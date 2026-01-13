/*
 * FocalTech FT9362 NN matching implementation
 * Copyright (C) 2025-2026 Ivan Kovalev
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define FT_USE_GLIB 1
#include <glib.h>

#include "focaltech_nn_match.h"
#include "focaltech_nn_infer.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <float.h>

#ifndef G_PI
#define G_PI 3.14159265358979323846
#endif

#ifdef FT_USE_GLIB
#define ft_malloc(size)  g_malloc (size)
#define ft_free(ptr)     g_free (ptr)
#else
#define ft_malloc(size)  malloc (size)
#define ft_free(ptr)     free (ptr)
#endif

void
ft_nn_match_init (FtNNMatchContext *ctx)
{
  ctx->nn_threshold = 0.2f;
  ctx->orientation_threshold = 35.0f;
  ctx->pixel_corr_threshold = 0.01f;
  ctx->tta_vote_threshold = 0.75f;
  ctx->min_agreeing_templates = 3;

  ctx->use_orientation_check = TRUE;
  ctx->use_tta = TRUE;
  ctx->use_pixel_correlation = TRUE;
}

static gint
float_compare (const void *a, const void *b)
{
  gfloat fa = *(const gfloat *)a;
  gfloat fb = *(const gfloat *)b;
  return (fa > fb) - (fa < fb);
}

static gfloat
compute_percentile (const gfloat *arr, gint size, gfloat percentile)
{
  gfloat *sorted;
  gfloat idx, frac, result;
  gint lo, hi;

  if (arr == NULL || size <= 0)
    return 0.0f;

  sorted = (gfloat *) ft_malloc (size * sizeof (gfloat));
  if (sorted == NULL)
    return 0.0f;

  memcpy (sorted, arr, size * sizeof (gfloat));
  qsort (sorted, size, sizeof (gfloat), float_compare);

  idx = (percentile / 100.0f) * (size - 1);
  lo = (gint) idx;
  hi = lo + 1;
  if (hi >= size)
    hi = size - 1;

  frac = idx - lo;
  result = sorted[lo] * (1 - frac) + sorted[hi] * frac;

  ft_free (sorted);
  return result;
}

static void
median_filter_3x3 (const gfloat *input, gfloat *output, gint height, gint width)
{
  gint y, x, dy, dx, ny, nx, count, i, j;
  gfloat window[9];
  gfloat tmp;

  for (y = 0; y < height; y++)
    {
      for (x = 0; x < width; x++)
        {
          count = 0;

          for (dy = -1; dy <= 1; dy++)
            {
              for (dx = -1; dx <= 1; dx++)
                {
                  ny = y + dy;
                  nx = x + dx;
                  if (ny >= 0 && ny < height && nx >= 0 && nx < width)
                    window[count++] = input[ny * width + nx];
                }
            }

          for (i = 0; i < count - 1; i++)
            {
              for (j = i + 1; j < count; j++)
                {
                  if (window[j] < window[i])
                    {
                      tmp = window[i];
                      window[i] = window[j];
                      window[j] = tmp;
                    }
                }
            }
          output[y * width + x] = window[count / 2];
        }
    }
}

void
ft_nn_process_raw (const unsigned char *raw_data, gfloat *output)
{
  const int16_t *pixels = (const int16_t *) (raw_data + FT_RAW_HEADER);
  gfloat temp[FT_NN_INPUT_HEIGHT * FT_NN_INPUT_WIDTH];
  gfloat filtered[FT_NN_INPUT_HEIGHT * FT_NN_INPUT_WIDTH];
  gfloat p5, p95, range, val;
  gint i;

  for (i = 0; i < FT_NN_INPUT_HEIGHT * FT_NN_INPUT_WIDTH; i++)
    temp[i] = (gfloat) pixels[3040 + i];

  median_filter_3x3 (temp, filtered, FT_NN_INPUT_HEIGHT, FT_NN_INPUT_WIDTH);

  p5 = compute_percentile (filtered, FT_NN_INPUT_SIZE, 5.0f);
  p95 = compute_percentile (filtered, FT_NN_INPUT_SIZE, 95.0f);
  range = p95 - p5 + 1e-8f;

  for (i = 0; i < FT_NN_INPUT_SIZE; i++)
    {
      val = (filtered[i] - p5) / range;
      if (val < 0.0f)
        val = 0.0f;
      if (val > 1.0f)
        val = 1.0f;
      output[i] = 1.0f - val;
    }
}

gfloat
ft_nn_compute_orientation (const gfloat *image)
{
  gfloat sum_gxx = 0.0f;
  gfloat sum_gyy = 0.0f;
  gfloat sum_gxy = 0.0f;
  gfloat gx, gy, angle_rad;
  gint y, x;

  for (y = 1; y < FT_NN_INPUT_HEIGHT - 1; y++)
    {
      for (x = 1; x < FT_NN_INPUT_WIDTH - 1; x++)
        {
          gx = -1.0f * image[(y-1) * FT_NN_INPUT_WIDTH + (x-1)] +
                1.0f * image[(y-1) * FT_NN_INPUT_WIDTH + (x+1)] +
               -2.0f * image[(y  ) * FT_NN_INPUT_WIDTH + (x-1)] +
                2.0f * image[(y  ) * FT_NN_INPUT_WIDTH + (x+1)] +
               -1.0f * image[(y+1) * FT_NN_INPUT_WIDTH + (x-1)] +
                1.0f * image[(y+1) * FT_NN_INPUT_WIDTH + (x+1)];

          gy = -1.0f * image[(y-1) * FT_NN_INPUT_WIDTH + (x-1)] +
               -2.0f * image[(y-1) * FT_NN_INPUT_WIDTH + (x  )] +
               -1.0f * image[(y-1) * FT_NN_INPUT_WIDTH + (x+1)] +
                1.0f * image[(y+1) * FT_NN_INPUT_WIDTH + (x-1)] +
                2.0f * image[(y+1) * FT_NN_INPUT_WIDTH + (x  )] +
                1.0f * image[(y+1) * FT_NN_INPUT_WIDTH + (x+1)];

          sum_gxx += gx * gx;
          sum_gyy += gy * gy;
          sum_gxy += gx * gy;
        }
    }

  angle_rad = 0.5f * atan2f (2.0f * sum_gxy, sum_gxx - sum_gyy);
  return angle_rad * 180.0f / (gfloat) G_PI;
}

gfloat
ft_nn_orientation_diff (gfloat angle1, gfloat angle2)
{
  gfloat diff = fabsf (angle1 - angle2);
  diff = fmodf (diff, 180.0f);
  if (diff > 90.0f)
    diff = 180.0f - diff;
  return diff;
}

gfloat
ft_nn_compute_ncc (const gfloat *img1, const gfloat *img2)
{
  gfloat mean1 = 0.0f, mean2 = 0.0f;
  gfloat std1 = 0.0f, std2 = 0.0f, corr = 0.0f;
  gfloat d1, d2;
  gint i;

  for (i = 0; i < FT_NN_INPUT_SIZE; i++)
    {
      mean1 += img1[i];
      mean2 += img2[i];
    }
  mean1 /= FT_NN_INPUT_SIZE;
  mean2 /= FT_NN_INPUT_SIZE;

  for (i = 0; i < FT_NN_INPUT_SIZE; i++)
    {
      d1 = img1[i] - mean1;
      d2 = img2[i] - mean2;
      std1 += d1 * d1;
      std2 += d2 * d2;
      corr += d1 * d2;
    }

  std1 = sqrtf (std1 / FT_NN_INPUT_SIZE + 1e-8f);
  std2 = sqrtf (std2 / FT_NN_INPUT_SIZE + 1e-8f);

  return corr / (FT_NN_INPUT_SIZE * std1 * std2);
}

#define QUALITY_MIN_CONTRAST      0.5f
#define QUALITY_MIN_VARIANCE      0.02f
#define QUALITY_MIN_STD           0.10f
#define QUALITY_MIN_CENTER_RATIO  0.15f
#define QUALITY_MIN_GABOR         0.01f
#define QUALITY_MIN_COHERENCE     0.0f

#define GABOR_NUM_ORIENT  8
#define GABOR_SIGMA       4.0f
#define GABOR_WAVELENGTH  8.0f
#define GABOR_KSIZE       17

static gfloat gabor_kernels[GABOR_NUM_ORIENT][GABOR_KSIZE * GABOR_KSIZE];
static gfloat gabor_angles[GABOR_NUM_ORIENT];

static gpointer
init_gabor_kernels_impl (gpointer data)
{
  gint half = GABOR_KSIZE / 2;
  gfloat freq = 1.0f / GABOR_WAVELENGTH;
  gint o, ky, kx, i;
  gfloat theta, sum_abs, x_theta, y_theta, gaussian, sinusoid, val;

  (void) data;

  for (o = 0; o < GABOR_NUM_ORIENT; o++)
    {
      theta = (gfloat) o * G_PI / GABOR_NUM_ORIENT;
      gabor_angles[o] = theta;

      sum_abs = 0.0f;
      for (ky = -half; ky <= half; ky++)
        {
          for (kx = -half; kx <= half; kx++)
            {
              x_theta = kx * cosf (theta) + ky * sinf (theta);
              y_theta = -kx * sinf (theta) + ky * cosf (theta);
              gaussian = expf (-(x_theta * x_theta + y_theta * y_theta) /
                              (2 * GABOR_SIGMA * GABOR_SIGMA));
              sinusoid = cosf (2 * G_PI * freq * x_theta);
              val = gaussian * sinusoid;
              gabor_kernels[o][(ky + half) * GABOR_KSIZE + (kx + half)] = val;
              sum_abs += fabsf (val);
            }
        }

      for (i = 0; i < GABOR_KSIZE * GABOR_KSIZE; i++)
        gabor_kernels[o][i] /= (sum_abs + 1e-8f);
    }

  return NULL;
}

static void
init_gabor_kernels (void)
{
  static GOnce gabor_init_once = G_ONCE_INIT;
  g_once (&gabor_init_once, init_gabor_kernels_impl, NULL);
}

static gfloat
convolve_at (const gfloat *img, gint h, gint w, gint y, gint x,
             const gfloat *kernel, gint ksize)
{
  gint half = ksize / 2;
  gfloat sum = 0.0f;
  gint ky, kx, iy, ix;

  for (ky = -half; ky <= half; ky++)
    {
      for (kx = -half; kx <= half; kx++)
        {
          iy = y + ky;
          ix = x + kx;
          if (iy >= 0 && iy < h && ix >= 0 && ix < w)
            sum += img[iy * w + ix] * kernel[(ky + half) * ksize + (kx + half)];
        }
    }
  return sum;
}

gboolean
ft_nn_check_quality (const gfloat *image)
{
  const gint h = FT_NN_INPUT_HEIGHT;
  const gint w = FT_NN_INPUT_WIDTH;
  gfloat sorted[FT_NN_INPUT_SIZE];
  gfloat p2, p98, contrast_range;
  gfloat mean = 0.0f, variance = 0.0f, d;
  gfloat cx, cy, sigma, range_val;
  gfloat total_energy = 0.0f, weighted_energy = 0.0f;
  gfloat weighted_sum = 0.0f, weighted_sq_sum = 0.0f;
  gfloat dx, dy, weight, val, stretched;
  gfloat center_ratio, mean_weighted, var_weighted, std_dev;
  gfloat img_std, gabor_sum = 0.0f, gabor_strength;
  gfloat coherence_sum = 0.0f, orient_coherence;
  gfloat max_resp, resp, center_orient, local_coh, neighbor_orient;
  gfloat img_norm[FT_NN_INPUT_SIZE];
  gfloat *orientation;
  gint i, y, x, N, gabor_count = 0, max_orient, o;
  gint block_size, coherence_count = 0, n_count;
  gint by, bx, ny, nx, n;
  gint neighbors[4][2];

  memcpy (sorted, image, FT_NN_INPUT_SIZE * sizeof (gfloat));
  qsort (sorted, FT_NN_INPUT_SIZE, sizeof (gfloat), float_compare);
  p2 = sorted[(gint) (0.02f * FT_NN_INPUT_SIZE)];
  p98 = sorted[(gint) (0.98f * FT_NN_INPUT_SIZE)];
  contrast_range = p98 - p2;

#ifdef FT_USE_GLIB
  g_debug ("Quality: contrast=%.3f (min=%.3f)", contrast_range, QUALITY_MIN_CONTRAST);
#endif
  if (contrast_range < QUALITY_MIN_CONTRAST)
    {
#ifdef FT_USE_GLIB
      g_debug ("Quality FAIL: contrast");
#endif
      return FALSE;
    }

  for (i = 0; i < FT_NN_INPUT_SIZE; i++)
    mean += image[i];
  mean /= FT_NN_INPUT_SIZE;

  for (i = 0; i < FT_NN_INPUT_SIZE; i++)
    {
      d = image[i] - mean;
      variance += d * d;
    }
  variance /= FT_NN_INPUT_SIZE;

#ifdef FT_USE_GLIB
  g_debug ("Quality: variance=%.4f (min=%.4f)", variance, QUALITY_MIN_VARIANCE);
#endif
  if (variance < QUALITY_MIN_VARIANCE)
    {
#ifdef FT_USE_GLIB
      g_debug ("Quality FAIL: variance");
#endif
      return FALSE;
    }

  cx = w / 2.0f;
  cy = h / 2.0f;
  sigma = (h < w ? h : w) / 3.0f;

  range_val = (p98 - p2) > 1e-8f ? (p98 - p2) : 1.0f;

  for (y = 0; y < h; y++)
    {
      for (x = 0; x < w; x++)
        {
          dx = x - cx;
          dy = y - cy;
          weight = expf (-(dx * dx + dy * dy) / (2 * sigma * sigma));

          val = image[y * w + x];
          stretched = (val - p2) / range_val;
          if (stretched < 0)
            stretched = 0;
          if (stretched > 1)
            stretched = 1;

          total_energy += stretched * stretched;
          weighted_energy += (stretched * weight) * (stretched * weight);
          weighted_sum += stretched * weight;
          weighted_sq_sum += (stretched * weight) * (stretched * weight);
        }
    }

  center_ratio = (total_energy > 1e-8f) ? (weighted_energy / total_energy) : 0.0f;
  N = h * w;
  mean_weighted = weighted_sum / N;
  var_weighted = (weighted_sq_sum / N) - (mean_weighted * mean_weighted);
  std_dev = sqrtf (var_weighted > 0 ? var_weighted : 0);

#ifdef FT_USE_GLIB
  g_debug ("Quality: std=%.3f (min=%.3f), center_ratio=%.3f (min=%.3f)",
           std_dev, QUALITY_MIN_STD, center_ratio, QUALITY_MIN_CENTER_RATIO);
#endif
  if (std_dev < QUALITY_MIN_STD || center_ratio < QUALITY_MIN_CENTER_RATIO)
    {
#ifdef FT_USE_GLIB
      g_debug ("Quality FAIL: std or center_ratio");
#endif
      return FALSE;
    }

  init_gabor_kernels ();

  img_std = sqrtf (variance);
  for (i = 0; i < FT_NN_INPUT_SIZE; i++)
    img_norm[i] = (image[i] - mean) / (img_std + 1e-8f);

  orientation = (gfloat *) ft_malloc (FT_NN_INPUT_SIZE * sizeof (gfloat));
  if (orientation == NULL)
    {
#ifdef FT_USE_GLIB
      g_debug ("Quality FAIL: memory allocation failed");
#endif
      return FALSE;
    }

  for (y = GABOR_KSIZE / 2; y < h - GABOR_KSIZE / 2; y++)
    {
      for (x = GABOR_KSIZE / 2; x < w - GABOR_KSIZE / 2; x++)
        {
          max_resp = 0.0f;
          max_orient = 0;

          for (o = 0; o < GABOR_NUM_ORIENT; o++)
            {
              resp = fabsf (convolve_at (img_norm, h, w, y, x,
                                         gabor_kernels[o], GABOR_KSIZE));
              if (resp > max_resp)
                {
                  max_resp = resp;
                  max_orient = o;
                }
            }

          gabor_sum += max_resp;
          gabor_count++;
          orientation[y * w + x] = gabor_angles[max_orient];
        }
    }

  gabor_strength = (gabor_count > 0) ? (gabor_sum / gabor_count) : 0.0f;

#ifdef FT_USE_GLIB
  g_debug ("Quality: gabor=%.4f (min=%.4f)", gabor_strength, QUALITY_MIN_GABOR);
#endif
  if (gabor_strength < QUALITY_MIN_GABOR)
    {
#ifdef FT_USE_GLIB
      g_debug ("Quality FAIL: gabor");
#endif
      ft_free (orientation);
      return FALSE;
    }

  block_size = 8;

  for (by = block_size + GABOR_KSIZE / 2; by < h - block_size - GABOR_KSIZE / 2; by += block_size)
    {
      for (bx = block_size + GABOR_KSIZE / 2; bx < w - block_size - GABOR_KSIZE / 2; bx += block_size)
        {
          center_orient = orientation[by * w + bx];
          local_coh = 0.0f;
          n_count = 0;

          neighbors[0][0] = by - block_size; neighbors[0][1] = bx;
          neighbors[1][0] = by + block_size; neighbors[1][1] = bx;
          neighbors[2][0] = by; neighbors[2][1] = bx - block_size;
          neighbors[3][0] = by; neighbors[3][1] = bx + block_size;

          for (n = 0; n < 4; n++)
            {
              ny = neighbors[n][0];
              nx = neighbors[n][1];
              if (ny >= GABOR_KSIZE / 2 && ny < h - GABOR_KSIZE / 2 &&
                  nx >= GABOR_KSIZE / 2 && nx < w - GABOR_KSIZE / 2)
                {
                  neighbor_orient = orientation[ny * w + nx];
                  local_coh += cosf (2.0f * (center_orient - neighbor_orient));
                  n_count++;
                }
            }

          if (n_count > 0)
            {
              coherence_sum += local_coh / n_count;
              coherence_count++;
            }
        }
    }

  ft_free (orientation);

  orient_coherence = (coherence_count > 0) ? (coherence_sum / coherence_count) : 0.0f;

#ifdef FT_USE_GLIB
  g_debug ("Quality: coherence=%.3f (min=%.3f)", orient_coherence, QUALITY_MIN_COHERENCE);
#endif
  if (orient_coherence < QUALITY_MIN_COHERENCE)
    {
#ifdef FT_USE_GLIB
      g_debug ("Quality FAIL: coherence");
#endif
      return FALSE;
    }

#ifdef FT_USE_GLIB
  g_debug ("Quality PASS");
#endif
  return TRUE;
}

gboolean
ft_nn_create_template (const gfloat *image, FtNNTemplate *tmpl)
{
  if (!ft_nn_check_quality (image))
    return FALSE;

  ft_nn_compute_embedding (image, tmpl->embedding);
  memcpy (tmpl->image, image, FT_NN_INPUT_SIZE * sizeof (gfloat));
  tmpl->orientation = ft_nn_compute_orientation (image);

  return TRUE;
}

static void
rotate_image (const gfloat *input, gfloat *output, gfloat angle_deg)
{
  gfloat angle_rad = angle_deg * (gfloat) G_PI / 180.0f;
  gfloat cos_a = cosf (angle_rad);
  gfloat sin_a = sinf (angle_rad);
  gfloat cx = FT_NN_INPUT_WIDTH / 2.0f;
  gfloat cy = FT_NN_INPUT_HEIGHT / 2.0f;
  gfloat dx, dy, sx, sy;
  gint y, x, ix, iy;

  for (y = 0; y < FT_NN_INPUT_HEIGHT; y++)
    {
      for (x = 0; x < FT_NN_INPUT_WIDTH; x++)
        {
          dx = x - cx;
          dy = y - cy;
          sx = dx * cos_a + dy * sin_a + cx;
          sy = -dx * sin_a + dy * cos_a + cy;

          ix = (gint) (sx + 0.5f);
          iy = (gint) (sy + 0.5f);

          if (ix >= 0 && ix < FT_NN_INPUT_WIDTH && iy >= 0 && iy < FT_NN_INPUT_HEIGHT)
            {
              output[y * FT_NN_INPUT_WIDTH + x] = input[iy * FT_NN_INPUT_WIDTH + ix];
            }
          else
            {
              ix = ix < 0 ? 0 : (ix >= FT_NN_INPUT_WIDTH ? FT_NN_INPUT_WIDTH - 1 : ix);
              iy = iy < 0 ? 0 : (iy >= FT_NN_INPUT_HEIGHT ? FT_NN_INPUT_HEIGHT - 1 : iy);
              output[y * FT_NN_INPUT_WIDTH + x] = input[iy * FT_NN_INPUT_WIDTH + ix];
            }
        }
    }
}

static void
shift_image (const gfloat *input, gfloat *output, gint dx, gint dy)
{
  gint y, x, sx, sy;

  for (y = 0; y < FT_NN_INPUT_HEIGHT; y++)
    {
      for (x = 0; x < FT_NN_INPUT_WIDTH; x++)
        {
          sx = x - dx;
          sy = y - dy;

          if (sx < 0)
            sx = 0;
          if (sx >= FT_NN_INPUT_WIDTH)
            sx = FT_NN_INPUT_WIDTH - 1;
          if (sy < 0)
            sy = 0;
          if (sy >= FT_NN_INPUT_HEIGHT)
            sy = FT_NN_INPUT_HEIGHT - 1;

          output[y * FT_NN_INPUT_WIDTH + x] = input[sy * FT_NN_INPUT_WIDTH + sx];
        }
    }
}

static void
adjust_brightness (const gfloat *input, gfloat *output, gfloat delta)
{
  gint i;
  gfloat val;

  for (i = 0; i < FT_NN_INPUT_SIZE; i++)
    {
      val = input[i] + delta;
      if (val < 0.0f)
        val = 0.0f;
      if (val > 1.0f)
        val = 1.0f;
      output[i] = val;
    }
}

static gint
compute_tta_votes (const gfloat *probe_image, const FtNNTemplate *templates,
                   gint num_templates, gfloat threshold)
{
  static const gfloat rotations[] = {-10.0f, -5.0f, 5.0f, 10.0f};
  static const gint shifts[][2] = {{-2, 0}, {2, 0}, {0, -2}, {0, 2}};
  static const gfloat brightness[] = {-0.05f, 0.05f};

  gfloat augmented[FT_NN_INPUT_SIZE];
  gfloat embedding[FT_NN_EMBEDDING_DIM];
  gint total_votes = 0;
  gint t, r, s, b;
  gfloat dist;

  ft_nn_compute_embedding (probe_image, embedding);
  for (t = 0; t < num_templates; t++)
    {
      dist = ft_nn_embedding_distance (embedding, templates[t].embedding);
      if (dist < threshold)
        {
          total_votes++;
          break;
        }
    }

  for (r = 0; r < 4; r++)
    {
      rotate_image (probe_image, augmented, rotations[r]);
      ft_nn_compute_embedding (augmented, embedding);

      for (t = 0; t < num_templates; t++)
        {
          dist = ft_nn_embedding_distance (embedding, templates[t].embedding);
          if (dist < threshold)
            {
              total_votes++;
              break;
            }
        }
    }

  for (s = 0; s < 4; s++)
    {
      shift_image (probe_image, augmented, shifts[s][0], shifts[s][1]);
      ft_nn_compute_embedding (augmented, embedding);

      for (t = 0; t < num_templates; t++)
        {
          dist = ft_nn_embedding_distance (embedding, templates[t].embedding);
          if (dist < threshold)
            {
              total_votes++;
              break;
            }
        }
    }

  for (b = 0; b < 2; b++)
    {
      adjust_brightness (probe_image, augmented, brightness[b]);
      ft_nn_compute_embedding (augmented, embedding);

      for (t = 0; t < num_templates; t++)
        {
          dist = ft_nn_embedding_distance (embedding, templates[t].embedding);
          if (dist < threshold)
            {
              total_votes++;
              break;
            }
        }
    }

  return total_votes;
}

gboolean
ft_nn_verify (const FtNNMatchContext *ctx, const gfloat *probe_image,
              const FtNNTemplate *templates, gint num_templates,
              FtNNMatchResult *result)
{
  gfloat probe_embedding[FT_NN_EMBEDDING_DIM];
  gfloat dist, diff, tta_ratio;
  gint t;

  if (ctx == NULL || probe_image == NULL || result == NULL)
    return FALSE;

  memset (result, 0, sizeof (*result));
  result->matched = FALSE;
  result->best_distance = FLT_MAX;
  result->best_template_idx = -1;
  result->tta_total = 11;

  if (num_templates == 0 || templates == NULL)
    return FALSE;

  result->probe_orientation = ft_nn_compute_orientation (probe_image);
  result->min_orientation_diff = FLT_MAX;

  if (ctx->use_orientation_check)
    {
      for (t = 0; t < num_templates; t++)
        {
          diff = ft_nn_orientation_diff (result->probe_orientation,
                                         templates[t].orientation);
          if (diff < result->min_orientation_diff)
            result->min_orientation_diff = diff;
        }

      if (result->min_orientation_diff > ctx->orientation_threshold)
        return FALSE;
    }

  ft_nn_compute_embedding (probe_image, probe_embedding);

  for (t = 0; t < num_templates; t++)
    {
      dist = ft_nn_embedding_distance (probe_embedding, templates[t].embedding);

      if (dist < result->best_distance)
        {
          result->best_distance = dist;
          result->best_template_idx = t;
        }

      if (dist < ctx->nn_threshold)
        result->templates_below_threshold++;
    }

  if (result->best_distance >= ctx->nn_threshold)
    return FALSE;

  if (result->templates_below_threshold < ctx->min_agreeing_templates)
    return FALSE;

  if (ctx->use_tta)
    {
      result->tta_votes = compute_tta_votes (probe_image, templates,
                                             num_templates, ctx->nn_threshold);

      tta_ratio = (gfloat) result->tta_votes / result->tta_total;
      if (tta_ratio < ctx->tta_vote_threshold)
        return FALSE;
    }
  else
    {
      result->tta_votes = result->tta_total;
    }

  if (ctx->use_pixel_correlation && result->best_template_idx >= 0)
    {
      result->best_ncc = ft_nn_compute_ncc (probe_image,
                                            templates[result->best_template_idx].image);

      if (result->best_ncc < ctx->pixel_corr_threshold)
        return FALSE;
    }
  else
    {
      result->best_ncc = 1.0f;
    }

  result->matched = TRUE;
  return TRUE;
}

size_t
ft_nn_template_serialize (const FtNNTemplate *tmpl, unsigned char *buffer)
{
  memcpy (buffer, tmpl, sizeof (FtNNTemplate));
  return sizeof (FtNNTemplate);
}

size_t
ft_nn_template_deserialize (const unsigned char *buffer, size_t size,
                            FtNNTemplate *tmpl)
{
  if (size < sizeof (FtNNTemplate))
    return 0;
  memcpy (tmpl, buffer, sizeof (FtNNTemplate));
  return sizeof (FtNNTemplate);
}
