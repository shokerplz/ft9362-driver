/*
 * FocalTech FT9362 (2808:0752) fingerprint driver
 * Copyright (C) 2025-2026 Ivan Kovalev
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define FP_COMPONENT "focaltech0752"

#include "drivers_api.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

/* Use shared NN code */
#define FT_USE_GLIB
#include "focaltech_nn_match.h"

/* Device constants */
#define FOCALTECH_VENDOR_ID   0x2808
#define FOCALTECH_PRODUCT_ID  0x0752

#define EP_IN   0x81
#define EP_OUT  0x03

#define EP_IN_MAX_BUF_SIZE  64
#define RAW_IMAGE_SIZE      12166

/* Command definitions */
#define CMD_STATUS_POLL_LEN     7
#define CMD_CAPTURE_LEN         5
#define RESPONSE_LEN            7

#define RESP_STX                0x02
#define RESP_STATUS_TYPE        0x04
#define RESP_FINGER_PRESENT_POS 4
#define FINGER_PRESENT          0x01

#define POLL_INTERVAL_MS        50
#define NR_ENROLL_STAGES        15

/* Commands */
static const guint8 cmd_status_poll[] = { 0x02, 0x00, 0x03, 0x80, 0x02, 0x01, 0x80 };
static const guint8 cmd_capture[] = { 0x02, 0x00, 0x01, 0x81, 0x80 };

/* Driver state */
struct _FpiDeviceFocaltech0752
{
  FpDevice        parent;

  /* USB state */
  gboolean        deactivating;
  gboolean        finger_on_sensor;
  guint8         *raw_buffer;
  gsize           raw_buffer_len;
  guint           poll_timeout_id;

  /* Matcher context */
  FtNNMatchContext match_ctx;

  /* Enrollment state */
  FtNNTemplate   *enroll_templates;
  int             enroll_count;
  int             enroll_stage;

  /* Verification state */
  FtNNTemplate   *verify_templates;
  int             verify_count;

  /* Debug tracking */
  gchar          *debug_dir;
  guint64         debug_session_id;
};

G_DECLARE_FINAL_TYPE (FpiDeviceFocaltech0752, fpi_device_focaltech0752, FPI, DEVICE_FOCALTECH0752, FpDevice);
G_DEFINE_TYPE (FpiDeviceFocaltech0752, fpi_device_focaltech0752, FP_TYPE_DEVICE);

/* Forward declarations */
static void start_finger_detection (FpiDeviceFocaltech0752 *self);
static void capture_image (FpiDeviceFocaltech0752 *self);
static gboolean poll_timeout_cb (gpointer user_data);
static int serialize_templates (FtNNTemplate *templates, int count, uint8_t **out_data, size_t *out_len);
static int deserialize_templates (const uint8_t *data, size_t len, FtNNTemplate **out_templates, int *out_count);

static void
save_debug_pgm (const float *image, int width, int height, const char *filename)
{
  FILE *fp;

  if (!g_getenv ("FP_DEBUG_IMAGES"))
    return;

  fp = fopen (filename, "wb");
  if (fp)
    {
      fprintf (fp, "P5\n%d %d\n255\n", width, height);
      for (int i = 0; i < width * height; i++)
        {
          guint8 val = (guint8)(image[i] * 255.0f);
          fwrite (&val, 1, 1, fp);
        }
      fclose (fp);
      fp_dbg ("Saved debug image: %s", filename);
    }
}

static void
ensure_debug_dir (FpiDeviceFocaltech0752 *self, const gchar *finger_name)
{
  g_autofree gchar *base_dir = NULL;

  if (!g_getenv ("FP_DEBUG_IMAGES"))
    return;

  g_clear_pointer (&self->debug_dir, g_free);
  base_dir = g_strdup_printf ("/tmp/fprint-debug-nn/%s", finger_name ? finger_name : "unknown");

  if (g_mkdir_with_parents (base_dir, 0755) == 0)
    {
      self->debug_dir = g_strdup (base_dir);
      self->debug_session_id = g_get_real_time () / 1000000;
      fp_info ("DEBUG: Created directory: %s (session %lu)", base_dir, self->debug_session_id);
    }
}

/* Serialization header for versioning */
#define FT_NN_SERIAL_MAGIC   0x464E4E01  /* "FNN\x01" */
#define FT_NN_SERIAL_VERSION 1

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t num_templates;
  uint32_t template_size;
} FtNNSerialHeader;

static int
serialize_templates (FtNNTemplate *templates, int count, uint8_t **out_data, size_t *out_len)
{
  FtNNSerialHeader header = {
    .magic = FT_NN_SERIAL_MAGIC,
    .version = FT_NN_SERIAL_VERSION,
    .num_templates = count,
    .template_size = sizeof(FtNNTemplate),
  };

  size_t total_size = sizeof(header) + count * sizeof(FtNNTemplate);
  uint8_t *data = malloc (total_size);
  if (!data)
    return -1;

  memcpy (data, &header, sizeof(header));
  memcpy (data + sizeof(header), templates, count * sizeof(FtNNTemplate));

  *out_data = data;
  *out_len = total_size;
  return 0;
}

static int
deserialize_templates (const uint8_t *data, size_t len, FtNNTemplate **out_templates, int *out_count)
{
  if (len < sizeof(FtNNSerialHeader))
    return -1;

  FtNNSerialHeader header;
  memcpy (&header, data, sizeof(header));

  if (header.magic != FT_NN_SERIAL_MAGIC || header.version != FT_NN_SERIAL_VERSION)
    return -1;

  if (header.template_size != sizeof(FtNNTemplate))
    return -1;

  size_t expected_size = sizeof(header) + header.num_templates * sizeof(FtNNTemplate);
  if (len < expected_size)
    return -1;

  FtNNTemplate *templates = malloc (header.num_templates * sizeof(FtNNTemplate));
  if (!templates)
    return -1;

  memcpy (templates, data + sizeof(header), header.num_templates * sizeof(FtNNTemplate));

  *out_templates = templates;
  *out_count = header.num_templates;
  return 0;
}

static void
capture_read_cb (FpiUsbTransfer *transfer, FpDevice *dev, gpointer user_data, GError *error)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (dev);

  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        fpi_device_action_error (dev, error);
      else
        g_error_free (error);
      return;
    }

  /* Append data to buffer */
  gsize copy_len = MIN (transfer->actual_length, RAW_IMAGE_SIZE - self->raw_buffer_len);
  memcpy (self->raw_buffer + self->raw_buffer_len, transfer->buffer, copy_len);
  self->raw_buffer_len += copy_len;

  /* Check if complete */
  if (self->raw_buffer_len >= RAW_IMAGE_SIZE)
    {
      fp_info ("Fingerprint captured - processing...");

      /* Process raw data to normalized float image */
      float image[FT_NN_INPUT_SIZE];
      ft_nn_process_raw (self->raw_buffer, image);

      /* Quality check */
      if (!ft_nn_check_quality (image))
        {
          fp_dbg ("Image quality check failed");
          FpiDeviceAction action = fpi_device_get_current_action (dev);

          if (action == FPI_DEVICE_ACTION_ENROLL)
            {
              fpi_device_enroll_progress (dev, self->enroll_stage, NULL,
                fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
            }
          else if (action == FPI_DEVICE_ACTION_VERIFY)
            {
              fpi_device_verify_report (dev, FPI_MATCH_ERROR, NULL,
                fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
            }
          else if (action == FPI_DEVICE_ACTION_IDENTIFY)
            {
              fpi_device_identify_report (dev, NULL, NULL,
                fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
            }

          /* Continue polling */
          self->finger_on_sensor = FALSE;
          g_clear_pointer (&self->raw_buffer, g_free);
          self->raw_buffer_len = 0;
          start_finger_detection (self);
          return;
        }

      /* Handle based on current action */
      FpiDeviceAction action = fpi_device_get_current_action (dev);

      if (action == FPI_DEVICE_ACTION_ENROLL)
        {
          /* Create template */
          if (!ft_nn_create_template (image, &self->enroll_templates[self->enroll_count]))
            {
              fp_dbg ("Failed to create template");
              fpi_device_enroll_progress (dev, self->enroll_stage, NULL,
                fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));
              self->finger_on_sensor = FALSE;
              g_clear_pointer (&self->raw_buffer, g_free);
              self->raw_buffer_len = 0;
              start_finger_detection (self);
              return;
            }

          /* Debug: save enrollment image */
          if (self->debug_dir)
            {
              g_autofree gchar *filename = g_strdup_printf (
                "%s/enroll_%03d.pgm", self->debug_dir, self->enroll_count);
              save_debug_pgm (image, FT_NN_INPUT_WIDTH, FT_NN_INPUT_HEIGHT, filename);
            }

          self->enroll_count++;
          self->enroll_stage++;

          fp_dbg ("Enrolled template %d/%d", self->enroll_count, NR_ENROLL_STAGES);

          if (self->enroll_count >= NR_ENROLL_STAGES)
            {
              /* Enrollment complete - serialize and store */
              uint8_t *data;
              size_t data_len;

              if (serialize_templates (self->enroll_templates, self->enroll_count,
                                        &data, &data_len) == 0)
                {
                  FpPrint *enroll_template;
                  fpi_device_get_enroll_data (dev, &enroll_template);

                  FpPrint *print = fp_print_new (dev);
                  fpi_print_set_type (print, FPI_PRINT_RAW);
                  fpi_print_set_device_stored (print, FALSE);

                  g_object_set (print,
                                "finger", fp_print_get_finger (enroll_template),
                                "username", fp_print_get_username (enroll_template),
                                "description", fp_print_get_description (enroll_template),
                                NULL);

                  GVariant *data_var = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                                   data, data_len, 1);
                  g_object_set (print, "fpi-data", data_var, NULL);
                  free (data);

                  fpi_device_enroll_complete (dev, print, NULL);
                }
              else
                {
                  fpi_device_enroll_complete (dev, NULL,
                    fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
                }

              /* Cleanup */
              g_clear_pointer (&self->enroll_templates, g_free);
              self->enroll_count = 0;
            }
          else
            {
              /* Report progress and continue */
              fpi_device_enroll_progress (dev, self->enroll_stage, NULL, NULL);
              self->finger_on_sensor = FALSE;
              start_finger_detection (self);
            }
        }
      else if (action == FPI_DEVICE_ACTION_VERIFY)
        {
          /* Debug: save probe image */
          guint64 debug_probe_id = 0;
          if (self->debug_dir)
            {
              debug_probe_id = g_get_real_time () / 1000;
              g_autofree gchar *filename = g_strdup_printf (
                "%s/verify_%lu.pgm", self->debug_dir, debug_probe_id);
              save_debug_pgm (image, FT_NN_INPUT_WIDTH, FT_NN_INPUT_HEIGHT, filename);
            }

          /* Match using 5-stage pipeline */
          gint64 t_match_start = g_get_monotonic_time ();
          FtNNMatchResult result;
          gboolean matched = ft_nn_verify (&self->match_ctx, image,
                                        self->verify_templates, self->verify_count,
                                        &result);
          gint64 t_match_end = g_get_monotonic_time ();

          /* Log result */
          fp_dbg ("Verify: matched=%d dist=%.4f templates_below=%d tta=%d/%d ncc=%.4f time=%ldms",
                  matched, result.best_distance, result.templates_below_threshold,
                  result.tta_votes, result.tta_total, result.best_ncc,
                  (t_match_end - t_match_start) / 1000);

          if (self->debug_dir && debug_probe_id > 0)
            {
              fp_info ("=== VERIFY probe_id=%lu: %s (dist=%.4f tmpl=%d tta=%d/%d ncc=%.4f orient=%.1f) ===",
                       debug_probe_id, matched ? "MATCH" : "NO_MATCH",
                       result.best_distance, result.templates_below_threshold,
                       result.tta_votes, result.tta_total, result.best_ncc,
                       result.probe_orientation);
            }

          /* Get the print we're verifying against */
          FpPrint *print = NULL;
          fpi_device_get_verify_data (dev, &print);

          fpi_device_verify_report (dev,
                                    matched ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL,
                                    print,
                                    NULL);
          fpi_device_verify_complete (dev, NULL);

          /* Cleanup */
          g_clear_pointer (&self->verify_templates, g_free);
          self->verify_count = 0;
        }
      else if (action == FPI_DEVICE_ACTION_IDENTIFY)
        {
          /* Match against all enrolled prints */
          gint64 t_identify_start = g_get_monotonic_time ();
          GPtrArray *prints;
          fpi_device_get_identify_data (dev, &prints);

          FpPrint *matched_print = NULL;
          float best_distance = 1e30f;

          for (guint i = 0; i < prints->len; i++)
            {
              FpPrint *print = g_ptr_array_index (prints, i);
              GVariant *data_var;
              g_object_get (print, "fpi-data", &data_var, NULL);

              if (!data_var)
                continue;

              gsize data_len;
              const uint8_t *data = g_variant_get_fixed_array (data_var, &data_len, 1);

              FtNNTemplate *templates;
              int count;

              if (deserialize_templates (data, data_len, &templates, &count) == 0)
                {
                  FtNNMatchResult result;
                  if (ft_nn_verify (&self->match_ctx, image, templates, count, &result))
                    {
                      if (result.best_distance < best_distance)
                        {
                          best_distance = result.best_distance;
                          matched_print = print;
                        }
                    }
                  free (templates);
                }

              g_variant_unref (data_var);
            }
          gint64 t_identify_end = g_get_monotonic_time ();

          fp_dbg ("Identify: %s, best_dist=%.4f, prints=%u, time=%ldms",
                  matched_print ? "MATCH" : "NO_MATCH", best_distance, prints->len,
                  (t_identify_end - t_identify_start) / 1000);

          if (matched_print)
            fpi_device_identify_report (dev, matched_print, NULL, NULL);
          else
            fpi_device_identify_report (dev, NULL, NULL, NULL);

          fpi_device_identify_complete (dev, NULL);
        }

      g_clear_pointer (&self->raw_buffer, g_free);
      self->raw_buffer_len = 0;
    }
  else
    {
      /* Need more data */
      FpiUsbTransfer *read_transfer = fpi_usb_transfer_new (dev);
      fpi_usb_transfer_fill_bulk (read_transfer, EP_IN, RAW_IMAGE_SIZE);
      fpi_usb_transfer_submit (read_transfer, 5000, NULL, capture_read_cb, NULL);
    }
}

static void
capture_cmd_cb (FpiUsbTransfer *transfer, FpDevice *dev, gpointer user_data, GError *error)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (dev);
  FpiUsbTransfer *read_transfer;

  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        fpi_device_action_error (dev, error);
      else
        g_error_free (error);
      return;
    }

  /* Initialize buffer */
  g_clear_pointer (&self->raw_buffer, g_free);
  self->raw_buffer = g_malloc (RAW_IMAGE_SIZE);
  self->raw_buffer_len = 0;

  /* Read image data */
  read_transfer = fpi_usb_transfer_new (dev);
  fpi_usb_transfer_fill_bulk (read_transfer, EP_IN, RAW_IMAGE_SIZE);
  fpi_usb_transfer_submit (read_transfer, 5000, NULL, capture_read_cb, NULL);
}

static void
capture_image (FpiDeviceFocaltech0752 *self)
{
  FpiUsbTransfer *transfer;

  fp_dbg ("Starting image capture");

  transfer = fpi_usb_transfer_new (FP_DEVICE (self));
  fpi_usb_transfer_fill_bulk_full (transfer, EP_OUT, (guint8 *) cmd_capture, CMD_CAPTURE_LEN, NULL);
  fpi_usb_transfer_submit (transfer, 1000, NULL, capture_cmd_cb, NULL);
}

static void
poll_status_cb (FpiUsbTransfer *transfer, FpDevice *dev, gpointer user_data, GError *error)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (dev);

  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        fpi_device_action_error (dev, error);
      else
        g_error_free (error);
      return;
    }

  if (self->deactivating)
    return;

  /* Check response */
  if (transfer->actual_length >= RESPONSE_LEN &&
      transfer->buffer[0] == RESP_STX &&
      transfer->buffer[3] == RESP_STATUS_TYPE)
    {
      guint8 finger_status = transfer->buffer[RESP_FINGER_PRESENT_POS];

      if (finger_status == FINGER_PRESENT && !self->finger_on_sensor)
        {
          fp_dbg ("Finger detected!");
          self->finger_on_sensor = TRUE;
          capture_image (self);
          return;
        }
      else if (finger_status != FINGER_PRESENT)
        {
          self->finger_on_sensor = FALSE;
        }
    }

  /* Schedule next poll */
  if (!self->deactivating)
    self->poll_timeout_id = g_timeout_add (POLL_INTERVAL_MS, poll_timeout_cb, dev);
}

static gboolean
poll_timeout_cb (gpointer user_data)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (user_data);
  self->poll_timeout_id = 0;
  if (!self->deactivating)
    start_finger_detection (self);
  return G_SOURCE_REMOVE;
}

static void
poll_cmd_cb (FpiUsbTransfer *transfer, FpDevice *dev, gpointer user_data, GError *error)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (dev);
  FpiUsbTransfer *read_transfer;

  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        fpi_device_action_error (dev, error);
      else
        g_error_free (error);
      return;
    }

  if (self->deactivating)
    return;

  read_transfer = fpi_usb_transfer_new (dev);
  fpi_usb_transfer_fill_bulk (read_transfer, EP_IN, EP_IN_MAX_BUF_SIZE);
  fpi_usb_transfer_submit (read_transfer, 1000, NULL, poll_status_cb, NULL);
}

static void
start_finger_detection (FpiDeviceFocaltech0752 *self)
{
  FpiUsbTransfer *transfer;

  if (self->deactivating)
    return;

  transfer = fpi_usb_transfer_new (FP_DEVICE (self));
  fpi_usb_transfer_fill_bulk_full (transfer, EP_OUT, (guint8 *) cmd_status_poll, CMD_STATUS_POLL_LEN, NULL);
  fpi_usb_transfer_submit (transfer, 1000, NULL, poll_cmd_cb, NULL);
}

static void
dev_open (FpDevice *dev)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (dev);
  GError *error = NULL;

  fp_dbg ("Opening device");

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (dev), 0, 0, &error))
    {
      fpi_device_open_complete (dev, error);
      return;
    }

  /* Initialize matcher context */
  ft_nn_match_init (&self->match_ctx);

  fpi_device_open_complete (dev, NULL);
}

static void
dev_close (FpDevice *dev)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (dev);
  GError *error = NULL;

  fp_dbg ("Closing device");

  g_clear_pointer (&self->raw_buffer, g_free);
  g_clear_pointer (&self->enroll_templates, g_free);
  g_clear_pointer (&self->verify_templates, g_free);

  g_usb_device_release_interface (fpi_device_get_usb_device (dev), 0, 0, &error);
  fpi_device_close_complete (dev, error);
}

static const gchar *
finger_to_name (FpFinger finger)
{
  switch (finger)
    {
      case FP_FINGER_LEFT_THUMB:   return "left-thumb";
      case FP_FINGER_LEFT_INDEX:   return "left-index";
      case FP_FINGER_LEFT_MIDDLE:  return "left-middle";
      case FP_FINGER_LEFT_RING:    return "left-ring";
      case FP_FINGER_LEFT_LITTLE:  return "left-little";
      case FP_FINGER_RIGHT_THUMB:  return "right-thumb";
      case FP_FINGER_RIGHT_INDEX:  return "right-index";
      case FP_FINGER_RIGHT_MIDDLE: return "right-middle";
      case FP_FINGER_RIGHT_RING:   return "right-ring";
      case FP_FINGER_RIGHT_LITTLE: return "right-little";
      default:                     return "unknown";
    }
}

static void
dev_enroll (FpDevice *dev)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (dev);
  FpPrint *enroll_template;

  fp_dbg ("Starting enrollment");

  self->deactivating = FALSE;
  self->finger_on_sensor = FALSE;
  self->enroll_templates = g_malloc0 (NR_ENROLL_STAGES * sizeof(FtNNTemplate));
  self->enroll_count = 0;
  self->enroll_stage = 0;

  fpi_device_get_enroll_data (dev, &enroll_template);
  ensure_debug_dir (self, finger_to_name (fp_print_get_finger (enroll_template)));

  start_finger_detection (self);
}

static void
dev_verify (FpDevice *dev)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (dev);
  FpPrint *print;
  GVariant *data_var;
  const uint8_t *data;
  gsize data_len;

  fp_info ("Starting verification - place finger on sensor");

  fpi_device_get_verify_data (dev, &print);
  g_object_get (print, "fpi-data", &data_var, NULL);

  if (!data_var)
    {
      fp_warn ("Verification failed: no print data");
      fpi_device_verify_complete (dev,
        fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
      return;
    }

  data = g_variant_get_fixed_array (data_var, &data_len, 1);

  if (deserialize_templates (data, data_len,
                              &self->verify_templates, &self->verify_count) != 0)
    {
      fp_warn ("Verification failed: invalid print format");
      g_variant_unref (data_var);
      fpi_device_verify_complete (dev,
        fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
      return;
    }

  fp_dbg ("Loaded %d templates for verification", self->verify_count);
  g_variant_unref (data_var);

  ensure_debug_dir (self, finger_to_name (fp_print_get_finger (print)));

  self->deactivating = FALSE;
  self->finger_on_sensor = FALSE;

  start_finger_detection (self);
}

static void
dev_identify (FpDevice *dev)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (dev);
  GPtrArray *prints;

  fpi_device_get_identify_data (dev, &prints);
  fp_info ("Starting identification against %u prints - place finger on sensor",
           prints ? prints->len : 0);

  ensure_debug_dir (self, "identify");

  self->deactivating = FALSE;
  self->finger_on_sensor = FALSE;

  start_finger_detection (self);
}

static void
dev_cancel (FpDevice *dev)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (dev);
  FpiDeviceAction action;
  g_autoptr(GError) error = NULL;

  fp_dbg ("Cancelling operation");

  self->deactivating = TRUE;

  if (self->poll_timeout_id != 0)
    {
      g_source_remove (self->poll_timeout_id);
      self->poll_timeout_id = 0;
    }

  action = fpi_device_get_current_action (dev);
  error = fpi_device_error_new (FP_DEVICE_ERROR_GENERAL);

  switch (action)
    {
    case FPI_DEVICE_ACTION_ENROLL:
      g_clear_pointer (&self->enroll_templates, g_free);
      self->enroll_count = 0;
      fpi_device_enroll_complete (dev, NULL, error);
      g_steal_pointer (&error);
      break;

    case FPI_DEVICE_ACTION_VERIFY:
      g_clear_pointer (&self->verify_templates, g_free);
      self->verify_count = 0;
      fpi_device_verify_complete (dev, error);
      g_steal_pointer (&error);
      break;

    case FPI_DEVICE_ACTION_IDENTIFY:
      fpi_device_identify_complete (dev, error);
      g_steal_pointer (&error);
      break;

    default:
      break;
    }
}

static const FpIdEntry id_table[] = {
  { .vid = FOCALTECH_VENDOR_ID, .pid = FOCALTECH_PRODUCT_ID },
  { .vid = 0, .pid = 0, .driver_data = 0 },
};

static void
fpi_device_focaltech0752_init (FpiDeviceFocaltech0752 *self)
{
  self->raw_buffer = NULL;
  self->raw_buffer_len = 0;
  self->deactivating = FALSE;
  self->finger_on_sensor = FALSE;
  self->poll_timeout_id = 0;
  self->enroll_templates = NULL;
  self->enroll_count = 0;
  self->verify_templates = NULL;
  self->verify_count = 0;
}

static void
fpi_device_focaltech0752_finalize (GObject *object)
{
  FpiDeviceFocaltech0752 *self = FPI_DEVICE_FOCALTECH0752 (object);

  if (self->poll_timeout_id != 0)
    g_source_remove (self->poll_timeout_id);

  g_clear_pointer (&self->raw_buffer, g_free);
  g_clear_pointer (&self->enroll_templates, g_free);
  g_clear_pointer (&self->verify_templates, g_free);
  g_clear_pointer (&self->debug_dir, g_free);

  G_OBJECT_CLASS (fpi_device_focaltech0752_parent_class)->finalize (object);
}

static void
fpi_device_focaltech0752_class_init (FpiDeviceFocaltech0752Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = fpi_device_focaltech0752_finalize;

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = "FocalTech FT9362 Fingerprint Sensor";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->nr_enroll_stages = NR_ENROLL_STAGES;
  dev_class->features = FP_DEVICE_FEATURE_IDENTIFY |
                        FP_DEVICE_FEATURE_VERIFY;

  dev_class->open = dev_open;
  dev_class->close = dev_close;
  dev_class->enroll = dev_enroll;
  dev_class->verify = dev_verify;
  dev_class->identify = dev_identify;
  dev_class->cancel = dev_cancel;
}
