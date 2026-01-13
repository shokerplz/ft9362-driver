/*
 * FocalTech FT9362 NN matching
 * Copyright (C) 2025-2026 Ivan Kovalev
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef FOCALTECH_NN_MATCH_H
#define FOCALTECH_NN_MATCH_H

#include "focaltech_nn_infer.h"

#ifdef FT_USE_GLIB
#include <glib.h>
#else
typedef int gboolean;
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#endif

/* Raw sensor data constants */
#define FT_RAW_HEADER   6

typedef struct {
  gfloat embedding[FT_NN_EMBEDDING_DIM];
  gfloat image[FT_NN_INPUT_SIZE];
  gfloat orientation;
} FtNNTemplate;

typedef struct {
  gfloat nn_threshold;
  gfloat orientation_threshold;
  gfloat pixel_corr_threshold;
  gfloat tta_vote_threshold;
  gint min_agreeing_templates;

  gboolean use_orientation_check;
  gboolean use_tta;
  gboolean use_pixel_correlation;
} FtNNMatchContext;

typedef struct {
  gboolean matched;
  gfloat best_distance;
  gint best_template_idx;
  gint templates_below_threshold;
  gint tta_votes;
  gint tta_total;
  gfloat best_ncc;
  gfloat probe_orientation;
  gfloat min_orientation_diff;
} FtNNMatchResult;

void ft_nn_match_init (FtNNMatchContext *ctx);

void ft_nn_process_raw (const unsigned char *raw_data, gfloat *output);

gfloat ft_nn_compute_orientation (const gfloat *image);

gfloat ft_nn_orientation_diff (gfloat angle1, gfloat angle2);

gfloat ft_nn_compute_ncc (const gfloat *img1, const gfloat *img2);

gboolean ft_nn_check_quality (const gfloat *image);

gboolean ft_nn_create_template (const gfloat *image, FtNNTemplate *tmpl);

gboolean ft_nn_verify (const FtNNMatchContext *ctx, const gfloat *probe_image,
                       const FtNNTemplate *templates, gint num_templates,
                       FtNNMatchResult *result);

size_t ft_nn_template_serialize (const FtNNTemplate *tmpl, unsigned char *buffer);

size_t ft_nn_template_deserialize (const unsigned char *buffer, size_t size,
                                   FtNNTemplate *tmpl);

#define FT_NN_TEMPLATE_SIZE (sizeof (FtNNTemplate))

#endif
