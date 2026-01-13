/* Auto-generated - DO NOT EDIT */
#ifndef FOCALTECH_NN_INFER_H
#define FOCALTECH_NN_INFER_H

#include <stddef.h>

/* Model constants */
#define FT_NN_INPUT_HEIGHT 76
#define FT_NN_INPUT_WIDTH 40
#define FT_NN_INPUT_SIZE (FT_NN_INPUT_HEIGHT * FT_NN_INPUT_WIDTH)
#define FT_NN_EMBEDDING_DIM 64

/**
 * Compute fingerprint embedding from preprocessed image.
 *
 * @param input Preprocessed image as float array [76][40], values in [0, 1]
 * @param output Output embedding array [64], will be L2-normalized
 */
void ft_nn_compute_embedding(const float *input, float *output);

/**
 * Compute L2 distance between two embeddings.
 *
 * @param emb1 First embedding [64]
 * @param emb2 Second embedding [64]
 * @return L2 distance (Euclidean distance)
 */
float ft_nn_embedding_distance(const float *emb1, const float *emb2);

#endif /* FOCALTECH_NN_INFER_H */
