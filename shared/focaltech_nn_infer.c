/* Auto-generated - DO NOT EDIT */
#include "focaltech_nn_infer.h"
#include "focaltech_nn_weights.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

/* Internal buffer sizes (after each pooling) */
#define CONV1_OUT_H 38
#define CONV1_OUT_W 20
#define CONV2_OUT_H 19
#define CONV2_OUT_W 10
#define CONV3_OUT_H 9
#define CONV3_OUT_W 5
#define CONV4_OUT_H 4
#define CONV4_OUT_W 2

/* Channel counts */
#define CONV1_OUT_CH 16
#define CONV2_OUT_CH 32
#define CONV3_OUT_CH 64
#define CONV4_OUT_CH 128

/* FC layer sizes */
#define FC1_IN (CONV4_OUT_CH * CONV4_OUT_H * CONV4_OUT_W)  /* 128 * 4 * 2 = 1024 */
#define FC1_OUT 256
#define FC2_OUT 64

/* Helper: ReLU activation */
static inline float relu(float x) {
    return x > 0.0f ? x : 0.0f;
}

/* Conv2d + ReLU: 3x3, padding=1, followed by 2x2 max pooling */
static void conv_bn_relu_pool(
    const float *input, int in_h, int in_w, int in_ch,
    const float *weight, const float *bias, int out_ch,
    float *output, int out_h, int out_w)
{
    /* Output after conv (before pooling) is same size as input due to padding=1 */
    int conv_h = in_h;
    int conv_w = in_w;

    /* Temporary buffer for conv output (before pooling) */
    float *conv_out = (float *)malloc(out_ch * conv_h * conv_w * sizeof(float));

    /* Conv2d with padding=1 */
    for (int oc = 0; oc < out_ch; oc++) {
        for (int oh = 0; oh < conv_h; oh++) {
            for (int ow = 0; ow < conv_w; ow++) {
                float sum = bias[oc];

                for (int ic = 0; ic < in_ch; ic++) {
                    for (int kh = 0; kh < 3; kh++) {
                        for (int kw = 0; kw < 3; kw++) {
                            int ih = oh + kh - 1;  /* padding=1 */
                            int iw = ow + kw - 1;

                            if (ih >= 0 && ih < in_h && iw >= 0 && iw < in_w) {
                                int in_idx = ic * in_h * in_w + ih * in_w + iw;
                                int w_idx = oc * in_ch * 9 + ic * 9 + kh * 3 + kw;
                                sum += input[in_idx] * weight[w_idx];
                            }
                        }
                    }
                }

                /* ReLU */
                conv_out[oc * conv_h * conv_w + oh * conv_w + ow] = relu(sum);
            }
        }
    }

    /* MaxPool2d with kernel=2, stride=2 */
    for (int oc = 0; oc < out_ch; oc++) {
        for (int oh = 0; oh < out_h; oh++) {
            for (int ow = 0; ow < out_w; ow++) {
                float max_val = -1e30f;

                for (int kh = 0; kh < 2; kh++) {
                    for (int kw = 0; kw < 2; kw++) {
                        int ih = oh * 2 + kh;
                        int iw = ow * 2 + kw;

                        if (ih < conv_h && iw < conv_w) {
                            float val = conv_out[oc * conv_h * conv_w + ih * conv_w + iw];
                            if (val > max_val) max_val = val;
                        }
                    }
                }

                output[oc * out_h * out_w + oh * out_w + ow] = max_val;
            }
        }
    }

    free(conv_out);
}

/* Fully connected layer + ReLU */
static void fc_relu(
    const float *input, int in_size,
    const float *weight, const float *bias, int out_size,
    float *output)
{
    for (int o = 0; o < out_size; o++) {
        float sum = bias[o];
        for (int i = 0; i < in_size; i++) {
            sum += input[i] * weight[o * in_size + i];
        }
        output[o] = relu(sum);
    }
}

/* Fully connected layer (no activation) */
static void fc(
    const float *input, int in_size,
    const float *weight, const float *bias, int out_size,
    float *output)
{
    for (int o = 0; o < out_size; o++) {
        float sum = bias[o];
        for (int i = 0; i < in_size; i++) {
            sum += input[i] * weight[o * in_size + i];
        }
        output[o] = sum;
    }
}

/* L2 normalize */
static void l2_normalize(float *vec, int size) {
    float norm = 0.0f;
    for (int i = 0; i < size; i++) {
        norm += vec[i] * vec[i];
    }
    norm = sqrtf(norm + 1e-8f);
    for (int i = 0; i < size; i++) {
        vec[i] /= norm;
    }
}

void ft_nn_compute_embedding(const float *input, float *output)
{
    /* Allocate intermediate buffers on stack */
    float buf1[CONV1_OUT_CH * CONV1_OUT_H * CONV1_OUT_W];  /* 16 * 38 * 20 = 12160 */
    float buf2[CONV2_OUT_CH * CONV2_OUT_H * CONV2_OUT_W];  /* 32 * 19 * 10 = 6080 */
    float buf3[CONV3_OUT_CH * CONV3_OUT_H * CONV3_OUT_W];  /* 64 * 9 * 5 = 2880 */
    float buf4[CONV4_OUT_CH * CONV4_OUT_H * CONV4_OUT_W];  /* 128 * 4 * 2 = 1024 */
    float fc1_out[FC1_OUT];  /* 256 */

    /* Conv1: (1, 76, 40) -> (16, 38, 20) */
    conv_bn_relu_pool(
        input, FT_NN_INPUT_HEIGHT, FT_NN_INPUT_WIDTH, 1,
        CONV1_WEIGHT, CONV1_BIAS, CONV1_OUT_CH,
        buf1, CONV1_OUT_H, CONV1_OUT_W);

    /* Conv2: (16, 38, 20) -> (32, 19, 10) */
    conv_bn_relu_pool(
        buf1, CONV1_OUT_H, CONV1_OUT_W, CONV1_OUT_CH,
        CONV2_WEIGHT, CONV2_BIAS, CONV2_OUT_CH,
        buf2, CONV2_OUT_H, CONV2_OUT_W);

    /* Conv3: (32, 19, 10) -> (64, 9, 5) */
    conv_bn_relu_pool(
        buf2, CONV2_OUT_H, CONV2_OUT_W, CONV2_OUT_CH,
        CONV3_WEIGHT, CONV3_BIAS, CONV3_OUT_CH,
        buf3, CONV3_OUT_H, CONV3_OUT_W);

    /* Conv4: (64, 9, 5) -> (128, 4, 2) */
    conv_bn_relu_pool(
        buf3, CONV3_OUT_H, CONV3_OUT_W, CONV3_OUT_CH,
        CONV4_WEIGHT, CONV4_BIAS, CONV4_OUT_CH,
        buf4, CONV4_OUT_H, CONV4_OUT_W);

    /* FC1: 1024 -> 256 (with ReLU) */
    fc_relu(buf4, FC1_IN, FC1_WEIGHT, FC1_BIAS, FC1_OUT, fc1_out);

    /* FC2: 256 -> 64 (no activation) */
    fc(fc1_out, FC1_OUT, FC2_WEIGHT, FC2_BIAS, FC2_OUT, output);

    /* L2 normalize */
    l2_normalize(output, FC2_OUT);
}

float ft_nn_embedding_distance(const float *emb1, const float *emb2)
{
    float sum = 0.0f;
    for (int i = 0; i < FT_NN_EMBEDDING_DIM; i++) {
        float diff = emb1[i] - emb2[i];
        sum += diff * diff;
    }
    return sqrtf(sum);
}
