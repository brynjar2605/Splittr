#include "layers.hpp"
#include "conv.hpp"
#include "lstm.hpp"
#include "model.hpp"
#include "tensor.hpp"
#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <unsupported/Eigen/CXX11/Tensor>
#include <vector>

#ifdef __APPLE__
// vDSP and vForce only. <Accelerate/Accelerate.h> drags in fortran_blas.h,
// whose prototypes conflict with the ones Eigen declares for EIGEN_USE_BLAS.
#include <vecLib/vDSP.h>
#include <vecLib/vForce.h>
#endif

namespace
{

// Eigen::Tensor3dXf is column-major, so x(i, c, w) lives at
// i + freq * c + freq * channels * w. That makes the (freq, channels) plane of
// a single w the contiguous unit, and the loop nest upstream used everywhere
// below — w innermost — walk one whole plane per element. The rewrites here
// all turn into straight walks over those planes, and these are the vector
// primitives they run over one: Accelerate on Apple, a plain loop elsewhere.

inline void vec_add_into(float *acc, const float *src, int n)
{
#ifdef __APPLE__
    vDSP_vadd(acc, 1, src, 1, acc, 1, (vDSP_Length)n);
#else
    for (int k = 0; k < n; ++k)
        acc[k] += src[k];
#endif
}

inline void vec_sub(float *dst, const float *src, const float *sub, int n)
{
#ifdef __APPLE__
    vDSP_vsub(sub, 1, src, 1, dst, 1, (vDSP_Length)n);
#else
    for (int k = 0; k < n; ++k)
        dst[k] = src[k] - sub[k];
#endif
}

inline void vec_square_add_into(float *acc, const float *src, int n)
{
#ifdef __APPLE__
    vDSP_vma(src, 1, src, 1, acc, 1, acc, 1, (vDSP_Length)n);
#else
    for (int k = 0; k < n; ++k)
        acc[k] += src[k] * src[k];
#endif
}

// dst = src * scale + shift, elementwise.
inline void vec_affine(float *dst, const float *src, const float *scale,
                       const float *shift, int n)
{
#ifdef __APPLE__
    vDSP_vma(src, 1, scale, 1, shift, 1, dst, 1, (vDSP_Length)n);
#else
    for (int k = 0; k < n; ++k)
        dst[k] = src[k] * scale[k] + shift[k];
#endif
}

// dst = src * scale + shift, one scale and shift for the whole run.
inline void vec_affine_scalar(float *dst, const float *src, float scale,
                              float shift, int n)
{
#ifdef __APPLE__
    vDSP_vsmsa(src, 1, &scale, &shift, dst, 1, (vDSP_Length)n);
#else
    for (int k = 0; k < n; ++k)
        dst[k] = src[k] * scale + shift;
#endif
}

// dst = a * sigmoid(b), which is a / (1 + exp(-b)). vForce has exp even
// though it has no erf, so this one does vectorize.
inline void vec_sigmoid_mul(float *dst, const float *a, const float *b,
                            float *scratch, int n)
{
#ifdef __APPLE__
    const float one = 1.0f;

    vDSP_vneg(b, 1, scratch, 1, (vDSP_Length)n);
    vvexpf(scratch, scratch, &n);
    vDSP_vsadd(scratch, 1, &one, scratch, 1, (vDSP_Length)n);
    vDSP_vdiv(scratch, 1, a, 1, dst, 1, (vDSP_Length)n);
#else
    (void)scratch;
    for (int k = 0; k < n; ++k)
        dst[k] = a[k] / (1.0f + std::exp(-b[k]));
#endif
}

// group_norm and group_norm_fused_gelu differ only in the activation, and both
// want the same three passes over the tensor: sum, sum of squares about the
// mean, then one affine write. Every call demucs makes passes num_groups == 1,
// but carrying the general case costs nothing here.
Eigen::Tensor3dXf group_norm_impl(const Eigen::Tensor3dXf &x,
                                  const Eigen::Tensor1dXf &weight,
                                  const Eigen::Tensor1dXf &b, int num_groups,
                                  float eps, bool fuse_gelu)
{
    const int freq = (int)x.dimension(0);
    const int channels = (int)x.dimension(1);
    const int width = (int)x.dimension(2);

    Eigen::Tensor3dXf y_out(freq, channels, width);

    const int group_size = channels / num_groups;
    const int plane = freq * channels;
    const int count = group_size * width; // elements behind one statistic

    const float *xp = x.data();
    float *yp = y_out.data();

    // Zeroed rather than merely sized: when channels does not divide evenly
    // into num_groups the trailing channels fall outside every group, and
    // upstream left exactly those at zero via y_out.setZero().
    std::vector<float> acc((size_t)plane, 0.0f);
    std::vector<float> mean((size_t)plane, 0.0f);
    std::vector<float> scale((size_t)plane, 0.0f);
    std::vector<float> shift((size_t)plane, 0.0f);
    std::vector<float> scratch((size_t)plane, 0.0f);

    for (int w = 0; w < width; ++w)
        vec_add_into(acc.data(), xp + (size_t)w * plane, plane);

    // Collapse each group's channels into one mean per frequency, then
    // broadcast it back across the plane so the variance pass needs no
    // indexing of its own.
    for (int g = 0; g < num_groups; ++g)
    {
        const int start = g * group_size;

        for (int i = 0; i < freq; ++i)
        {
            float sum = 0.0f;
            for (int c = start; c < start + group_size; ++c)
                sum += acc[(size_t)i + (size_t)freq * c];

            const float group_mean = sum / (float)count;

            for (int c = start; c < start + group_size; ++c)
                mean[(size_t)i + (size_t)freq * c] = group_mean;
        }
    }

    std::fill(acc.begin(), acc.end(), 0.0f);

    for (int w = 0; w < width; ++w)
    {
        vec_sub(scratch.data(), xp + (size_t)w * plane, mean.data(), plane);
        vec_square_add_into(acc.data(), scratch.data(), plane);
    }

    // Fold normalize, scale and shift into a single affine pass:
    // (x - mean) / sqrt(var + eps) * weight + b is the same as
    // x * (weight / sqrt(var + eps)) + (b - mean * weight / sqrt(var + eps)).
    for (int g = 0; g < num_groups; ++g)
    {
        const int start = g * group_size;

        for (int i = 0; i < freq; ++i)
        {
            float sum_squares = 0.0f;
            for (int c = start; c < start + group_size; ++c)
                sum_squares += acc[(size_t)i + (size_t)freq * c];

            const float inv_std =
                1.0f / std::sqrt(sum_squares / (float)(count - 1) + eps);

            for (int c = start; c < start + group_size; ++c)
            {
                const size_t k = (size_t)i + (size_t)freq * c;
                scale[k] = inv_std * weight(c);
                shift[k] = b(c) - mean[k] * scale[k];
            }
        }
    }

    for (int w = 0; w < width; ++w)
    {
        float *dst = yp + (size_t)w * plane;

        vec_affine(dst, xp + (size_t)w * plane, scale.data(), shift.data(),
                   plane);

        // vForce has no erf, so GeLU stays scalar — but it now runs over
        // contiguous memory instead of one strided element per plane.
        if (fuse_gelu)
            for (int k = 0; k < plane; ++k)
                dst[k] = 0.5f * dst[k] *
                         (1.0f + std::erf(dst[k] / std::sqrt(2.0f)));
    }

    return y_out;
}

} // namespace

Eigen::Tensor3dXf demucscpp::group_norm(const Eigen::Tensor3dXf &x,
                                        const Eigen::Tensor1dXf &weight,
                                        const Eigen::Tensor1dXf &b,
                                        int num_groups, float eps)
{
    return group_norm_impl(x, weight, b, num_groups, eps, false);
}

Eigen::Tensor3dXf
demucscpp::group_norm_fused_gelu(const Eigen::Tensor3dXf &x,
                                 const Eigen::Tensor1dXf &weight,
                                 const Eigen::Tensor1dXf &bias, float eps)
{
    // Normalizing over the entire channel since num_groups is always 1
    return group_norm_impl(x, weight, bias, 1, eps, true);
}

Eigen::Tensor3dXf demucscpp::glu(const Eigen::Tensor3dXf &x, const int dim)
{
    if (x.dimension(dim) % 2 != 0)
    {
        std::cerr << "Dimension size must be evenly divisible by 2"
                  << std::endl;
        std::exit(1);
    }

    const int split_size = (int)x.dimension(dim) / 2;

    Eigen::array<int, 3> sizes = {(int)x.dimension(0), (int)x.dimension(1),
                                  (int)x.dimension(2)};
    sizes[dim] = split_size;

    Eigen::Tensor3dXf y_out(sizes[0], sizes[1], sizes[2]);

    // Whichever dimension the split is on, both halves come out as contiguous
    // runs: the dimensions below the split make up a run of inner*split_size,
    // and the ones above simply repeat it. So neither half needs gathering,
    // and the sigmoid gets a straight vector to work on.
    int inner = 1;
    for (int d = 0; d < dim; ++d)
        inner *= (int)x.dimension(d);

    int outer = 1;
    for (int d = dim + 1; d < 3; ++d)
        outer *= (int)x.dimension(d);

    const int run = inner * split_size;

    const float *xp = x.data();
    float *yp = y_out.data();

    std::vector<float> scratch((size_t)run, 0.0f);

    for (int r = 0; r < outer; ++r)
    {
        const float *first = xp + (size_t)r * inner * (int)x.dimension(dim);

        vec_sigmoid_mul(yp + (size_t)r * run, first, first + run,
                        scratch.data(), run);
    }

    return y_out;
}

Eigen::Tensor3dXf demucscpp::layer_norm(const Eigen::Tensor3dXf &x,
                                        const Eigen::Tensor1dXf &weight,
                                        const Eigen::Tensor1dXf &bias,
                                        float eps)
{
    const int freq = (int)x.dimension(0);
    const int channels = (int)x.dimension(1);
    const int width = (int)x.dimension(2);

    Eigen::Tensor3dXf y_out(freq, channels, width);

    const int plane = freq * channels;

    const float *xp = x.data();
    float *yp = y_out.data();

    // This statistic runs along the last dimension, the one the layout
    // strides by a whole plane. Accumulating a plane at a time computes every
    // (i, c) mean at once and reads the tensor in order while doing it.
    std::vector<float> acc((size_t)plane, 0.0f);
    std::vector<float> mean((size_t)plane, 0.0f);
    std::vector<float> scale((size_t)plane, 0.0f);
    std::vector<float> shift((size_t)plane, 0.0f);
    std::vector<float> scratch((size_t)plane, 0.0f);

    for (int w = 0; w < width; ++w)
        vec_add_into(acc.data(), xp + (size_t)w * plane, plane);

    for (int k = 0; k < plane; ++k)
        mean[k] = acc[k] / (float)width;

    std::fill(acc.begin(), acc.end(), 0.0f);

    for (int w = 0; w < width; ++w)
    {
        vec_sub(scratch.data(), xp + (size_t)w * plane, mean.data(), plane);
        vec_square_add_into(acc.data(), scratch.data(), plane);
    }

    for (int k = 0; k < plane; ++k)
    {
        scale[k] = 1.0f / std::sqrt(acc[k] / (float)(width - 1) + eps);
        shift[k] = -mean[k] * scale[k];
    }

    // weight and bias index the last dimension, so unlike group_norm they are
    // a single scalar pair per plane rather than a vector across one.
    for (int w = 0; w < width; ++w)
    {
        vec_affine(scratch.data(), xp + (size_t)w * plane, scale.data(),
                   shift.data(), plane);
        vec_affine_scalar(yp + (size_t)w * plane, scratch.data(), weight(w),
                          bias(w), plane);
    }

    return y_out;
}

void demucscpp::apply_dconv(const struct demucscpp::demucs_model &model,
                            Eigen::Tensor3dXf &y, int freq_idx, int encdec_idx,
                            int layer_idx, int mid_crop)
{
    // store another copy of y to sum back later
    Eigen::Tensor3dXf y_copy = y;

    // now dconv time

    switch (layer_idx)
    {
    case 0:
        y = demucscpp::conv1d<48, 6, 3, 1, 1, 1>(
            y,
            model.dconv_layers_0_conv1d_weight[freq_idx][encdec_idx][layer_idx]
                                              [0],
            model.dconv_layers_0_conv1d_bias[freq_idx][encdec_idx][layer_idx]
                                            [0]);
        break;
    case 1:
        y = demucscpp::conv1d<96, 12, 3, 1, 1, 1>(
            y,
            model.dconv_layers_0_conv1d_weight[freq_idx][encdec_idx][layer_idx]
                                              [0],
            model.dconv_layers_0_conv1d_bias[freq_idx][encdec_idx][layer_idx]
                                            [0]);
        break;
    case 2:
        y = demucscpp::conv1d<192, 24, 3, 1, 1, 1>(
            y,
            model.dconv_layers_0_conv1d_weight[freq_idx][encdec_idx][layer_idx]
                                              [0],
            model.dconv_layers_0_conv1d_bias[freq_idx][encdec_idx][layer_idx]
                                            [0]);
        break;
    case 3:
        y = demucscpp::conv1d<384, 48, 3, 1, 1, 1>(
            y,
            model.dconv_layers_0_conv1d_weight[freq_idx][encdec_idx][layer_idx]
                                              [0],
            model.dconv_layers_0_conv1d_bias[freq_idx][encdec_idx][layer_idx]
                                            [0]);
        break;
    };

    y = demucscpp::group_norm_fused_gelu(
        y,
        model.dconv_layers_1_groupnorm_weight[freq_idx][encdec_idx][layer_idx]
                                             [0],
        model.dconv_layers_1_groupnorm_bias[freq_idx][encdec_idx][layer_idx][0],
        1e-05);

    switch (layer_idx)
    {
    case 0:
        y = demucscpp::conv1d<6, 96, 1, 1, 0, 1>(
            y,
            model.dconv_layers_3_conv1d_weight[freq_idx][encdec_idx][layer_idx]
                                              [0],
            model.dconv_layers_3_conv1d_bias[freq_idx][encdec_idx][layer_idx]
                                            [0]);
        break;
    case 1:
        y = demucscpp::conv1d<12, 192, 1, 1, 0, 1>(
            y,
            model.dconv_layers_3_conv1d_weight[freq_idx][encdec_idx][layer_idx]
                                              [0],
            model.dconv_layers_3_conv1d_bias[freq_idx][encdec_idx][layer_idx]
                                            [0]);
        break;
    case 2:
        y = demucscpp::conv1d<24, 384, 1, 1, 0, 1>(
            y,
            model.dconv_layers_3_conv1d_weight[freq_idx][encdec_idx][layer_idx]
                                              [0],
            model.dconv_layers_3_conv1d_bias[freq_idx][encdec_idx][layer_idx]
                                            [0]);
        break;
    case 3:
        y = demucscpp::conv1d<48, 768, 1, 1, 0, 1>(
            y,
            model.dconv_layers_3_conv1d_weight[freq_idx][encdec_idx][layer_idx]
                                              [0],
            model.dconv_layers_3_conv1d_bias[freq_idx][encdec_idx][layer_idx]
                                            [0]);
        break;
    };

    y = demucscpp::group_norm(
        y,
        model.dconv_layers_4_groupnorm_weight[freq_idx][encdec_idx][layer_idx]
                                             [0],
        model.dconv_layers_4_groupnorm_bias[freq_idx][encdec_idx][layer_idx][0],
        1, 1e-05);

    y = demucscpp::glu(y, 1);

    y = demucscpp::layer_scale(
        y, model.dconv_layers_6_scale[freq_idx][encdec_idx][layer_idx][0]);

    // now we add y to itself
    y = y + y_copy;

    // store another copy of y to sum back later
    y_copy = y;

    // NEXT ENTIRE SUBSEQUENCE OF DCONV WITH SLIGHTLY DIFFERENT PARAMS

    // Conv1d(48, 6, kernel_size=(3,), stride=(1,), padding=(2,), dilation=(2,))
    switch (layer_idx)
    {
    case 0:
        y = demucscpp::conv1d<48, 6, 3, 1, 2, 2>(
            y,
            model.dconv_layers_0_conv1d_weight[freq_idx][encdec_idx][layer_idx]
                                              [1],
            model.dconv_layers_0_conv1d_bias[freq_idx][encdec_idx][layer_idx]
                                            [1]);
        break;
    case 1:
        y = demucscpp::conv1d<96, 12, 3, 1, 2, 2>(
            y,
            model.dconv_layers_0_conv1d_weight[freq_idx][encdec_idx][layer_idx]
                                              [1],
            model.dconv_layers_0_conv1d_bias[freq_idx][encdec_idx][layer_idx]
                                            [1]);
        break;
    case 2:
        y = demucscpp::conv1d<192, 24, 3, 1, 2, 2>(
            y,
            model.dconv_layers_0_conv1d_weight[freq_idx][encdec_idx][layer_idx]
                                              [1],
            model.dconv_layers_0_conv1d_bias[freq_idx][encdec_idx][layer_idx]
                                            [1]);
        break;
    case 3:
        y = demucscpp::conv1d<384, 48, 3, 1, 2, 2>(
            y,
            model.dconv_layers_0_conv1d_weight[freq_idx][encdec_idx][layer_idx]
                                              [1],
            model.dconv_layers_0_conv1d_bias[freq_idx][encdec_idx][layer_idx]
                                            [1]);
        break;
    };

    Eigen::Tensor3dXf y_cropped =
        y.slice(Eigen::array<Eigen::Index, 3>({0, 0, 0}),
                Eigen::array<Eigen::Index, 3>(
                    {y.dimension(0), y.dimension(1), mid_crop}));

    y = y_cropped;

    y = demucscpp::group_norm_fused_gelu(
        y,
        model.dconv_layers_1_groupnorm_weight[freq_idx][encdec_idx][layer_idx]
                                             [1],
        model.dconv_layers_1_groupnorm_bias[freq_idx][encdec_idx][layer_idx][1],
        1e-05);

    // Conv1d(6, 96, kernel_size=(1,), stride=(1,))
    switch (layer_idx)
    {
    case 0:
        y = demucscpp::conv1d<6, 96, 1, 1, 0, 1>(
            y,
            model.dconv_layers_3_conv1d_weight[freq_idx][encdec_idx][layer_idx]
                                              [1],
            model.dconv_layers_3_conv1d_bias[freq_idx][encdec_idx][layer_idx]
                                            [1]);
        break;
    case 1:
        y = demucscpp::conv1d<12, 192, 1, 1, 0, 1>(
            y,
            model.dconv_layers_3_conv1d_weight[freq_idx][encdec_idx][layer_idx]
                                              [1],
            model.dconv_layers_3_conv1d_bias[freq_idx][encdec_idx][layer_idx]
                                            [1]);
        break;
    case 2:
        y = demucscpp::conv1d<24, 384, 1, 1, 0, 1>(
            y,
            model.dconv_layers_3_conv1d_weight[freq_idx][encdec_idx][layer_idx]
                                              [1],
            model.dconv_layers_3_conv1d_bias[freq_idx][encdec_idx][layer_idx]
                                            [1]);
        break;
    case 3:
        y = demucscpp::conv1d<48, 768, 1, 1, 0, 1>(
            y,
            model.dconv_layers_3_conv1d_weight[freq_idx][encdec_idx][layer_idx]
                                              [1],
            model.dconv_layers_3_conv1d_bias[freq_idx][encdec_idx][layer_idx]
                                            [1]);
        break;
    };

    y = demucscpp::group_norm(
        y,
        model.dconv_layers_4_groupnorm_weight[freq_idx][encdec_idx][layer_idx]
                                             [1],
        model.dconv_layers_4_groupnorm_bias[freq_idx][encdec_idx][layer_idx][1],
        1, 1e-05);

    y = demucscpp::glu(y, 1);
    y = demucscpp::layer_scale(
        y, model.dconv_layers_6_scale[freq_idx][encdec_idx][layer_idx][1]);

    // if y_copy is shorter than y in the last dim
    // pad the last dim with zeros to match

    if (y_copy.dimension(2) < y.dimension(2))
    {
        // pad the last dim with zeros to match
        Eigen::Tensor3dXf padded_tensor_copy(
            y_copy.dimension(0), y_copy.dimension(1), y.dimension(2));
        padded_tensor_copy.setZero();
        padded_tensor_copy.slice(Eigen::array<Eigen::Index, 3>({0, 0, 0}),
                                 y_copy.dimensions()) = y_copy;
        y_copy = padded_tensor_copy;
    }

    // now sum with itself
    y = y + y_copy;
}

void demucscpp::common_encoder_layer(
    Eigen::Tensor3dXf &q,       // q = x = frequency
    const Eigen::Tensor3dXf &k, // k = xt = time
    const Eigen::Tensor1dXf &norm1_weight, const Eigen::Tensor1dXf &norm1_bias,
    const Eigen::Tensor1dXf &norm2_weight, const Eigen::Tensor1dXf &norm2_bias,
    const Eigen::MatrixXf &in_proj_weight, const Eigen::VectorXf &in_proj_bias,
    const Eigen::MatrixXf &out_proj_weight,
    const Eigen::VectorXf &out_proj_bias, const Eigen::VectorXf &gamma1_scale,
    const Eigen::Tensor1dXf &norm3_weight, const Eigen::Tensor1dXf &norm3_bias,
    const Eigen::MatrixXf &linear1_weight, const Eigen::VectorXf &linear1_bias,
    const Eigen::MatrixXf &linear2_weight, const Eigen::VectorXf &linear2_bias,
    const Eigen::VectorXf &gamma2_scale,
    const Eigen::Tensor1dXf &norm_out_weight,
    const Eigen::Tensor1dXf &norm_out_bias, const int num_heads,
    // optional params
    float eps, const bool self_attention)
{
    // Normalize x using the norm1 weights and biases
    Eigen::Tensor3dXf q_norm =
        demucscpp::layer_norm(q, norm1_weight, norm1_bias, eps);

    Eigen::Tensor3dXf k_norm;
    if (self_attention)
    {
        k_norm = q_norm;
    }
    else
    {
        k_norm = demucscpp::layer_norm(k, norm2_weight, norm2_bias, eps);
    }

    // Cross-attention block
    // Compute Q, K, V matrices

    int B = q.dimension(0);
    int T = q.dimension(1);
    int C = q.dimension(2);

    int S = k.dimension(1);

    // Reshape q, k to 2D matrix of dimensions (T*B, C)

    // Use Eigen::Map to avoid manual loops for reshaping
    Eigen::MatrixXf q_norm_2d =
        Eigen::Map<const Eigen::MatrixXf>(q_norm.data(), T, C);
    Eigen::MatrixXf k_norm_2d =
        Eigen::Map<const Eigen::MatrixXf>(k_norm.data(), S, C);

    // Compute Q, K, V matrices
    Eigen::MatrixXf Q =
        q_norm_2d * in_proj_weight.block(0, 0, C, C).transpose();
    Eigen::MatrixXf K =
        k_norm_2d * in_proj_weight.block(C, 0, C, C).transpose();
    Eigen::MatrixXf V =
        k_norm_2d * in_proj_weight.block(2 * C, 0, C, C).transpose();

    Eigen::VectorXf q_bias = in_proj_bias.segment(0, C);
    Eigen::VectorXf k_bias = in_proj_bias.segment(C, C);
    Eigen::VectorXf v_bias = in_proj_bias.segment(2 * C, C);

    // copied from linear layer: ff1.rowwise() += linear1_bias.transpose();
    Q.rowwise() += q_bias.transpose();
    K.rowwise() += k_bias.transpose();
    V.rowwise() += v_bias.transpose();

    int head_split = C / num_heads;

    // map matrices to tensors
    Eigen::Tensor3dXf Q_heads =
        Eigen::TensorMap<Eigen::Tensor3dXf>(Q.data(), T, head_split, num_heads);
    Eigen::Tensor3dXf K_heads =
        Eigen::TensorMap<Eigen::Tensor3dXf>(K.data(), S, head_split, num_heads);
    Eigen::Tensor3dXf V_heads =
        Eigen::TensorMap<Eigen::Tensor3dXf>(V.data(), S, head_split, num_heads);

    Eigen::MatrixXf cross_attn_out(T, C);

    for (int h = 0; h < num_heads; ++h)
    {
        // Extract the h-th head from Q_heads and K_heads
        Eigen::Tensor2dXf Q_head_tensor = Q_heads.chip(h, 2);
        Eigen::Tensor2dXf K_head_tensor = K_heads.chip(h, 2);
        Eigen::Tensor2dXf V_head_tensor = V_heads.chip(h, 2);

        // Reshape the tensors to matrices
        Eigen::Map<Eigen::MatrixXf> Q_head(Q_head_tensor.data(), T, head_split);
        Eigen::Map<Eigen::MatrixXf> K_head(K_head_tensor.data(), S, head_split);
        Eigen::Map<Eigen::MatrixXf> V_head(V_head_tensor.data(), S, head_split);

        // Compute the dot product of Q_head and K_head
        Eigen::MatrixXf dot_product =
            Q_head * K_head.transpose() / std::sqrt((float)head_split);

        // Apply softmax to the dot product
        Eigen::ArrayXf max_vals = dot_product.rowwise().maxCoeff();
        Eigen::MatrixXf max_vals_expanded = max_vals.replicate(1, S);
        Eigen::MatrixXf softmax_scores =
            (dot_product - max_vals_expanded).array().exp().matrix();
        Eigen::VectorXf row_sums = softmax_scores.rowwise().sum();
        Eigen::MatrixXf divisor = row_sums.replicate(1, S);
        softmax_scores = (softmax_scores.array() / divisor.array()).matrix();

        Eigen::MatrixXf cross_attn_head = softmax_scores * V_head;
        cross_attn_out.block(0, h * head_split, T, head_split) =
            cross_attn_head;
    }

    // Copy q into q_2d (Map q to 2D matrix)
    Eigen::Map<Eigen::MatrixXf> q_2d(q.data(), T, C);

    // Apply output projection with gamma1_scale
    Eigen::MatrixXf out_proj = cross_attn_out * out_proj_weight.transpose();
    out_proj.array().rowwise() += out_proj_bias.transpose().array();
    out_proj = out_proj.array().rowwise() * gamma1_scale.transpose().array();

    // Add to q
    q_2d += out_proj;

    // before feedforward, apply norm3 to x i.e. q
    q_norm = demucscpp::layer_norm(q, norm3_weight, norm3_bias, eps);
    q_norm_2d = Eigen::Map<const Eigen::MatrixXf>(q_norm.data(), T, C);

    // Feedforward block
    // Linear layer 1
    Eigen::MatrixXf ff1 = q_norm_2d * linear1_weight.transpose();
    ff1.rowwise() += linear1_bias.transpose();

    ff1 = demucscpp::gelu(ff1);

    // Linear layer 2
    Eigen::MatrixXf ff2 = ff1 * linear2_weight.transpose();
    ff2.rowwise() += linear2_bias.transpose();

    // Apply gamma_2 scale directly on 2D matrix
    ff2 = ff2.array().rowwise() * gamma2_scale.transpose().array();

    // now x = x + self.gamma_2(self._ff_block(self.norm3(q))))
    q_2d += ff2;

    // Map the 2D data back into a 3D tensor with dimensions (T, B, C)
    q = Eigen::TensorMap<Eigen::Tensor3dXf>(q_2d.data(), T, B, C);

    // Swap the first and last dimensions to get a tensor with dimensions (B, C,
    // T)
    Eigen::array<int, 3> permute_dims_3 = {1, 2, 0};
    Eigen::Tensor3dXf q_shuf = q.shuffle(permute_dims_3);

    // Normalize the output with norm_out/MyGroupNorm
    q = demucscpp::group_norm(q_shuf, norm_out_weight, norm_out_bias, 1, eps);

    Eigen::array<int, 3> permute_dims_4 = {0, 2, 1};
    q_shuf = q.shuffle(permute_dims_4);

    q = q_shuf;
}

void demucscpp_v3::local_attention(
    Eigen::Tensor3dXf &x, // x = frequency, time, or combined
                          // input tensor [B, C, T]
    const Eigen::Tensor3dXf &content_weight,
    const Eigen::Tensor1dXf &content_bias,
    const Eigen::Tensor3dXf &query_weight, const Eigen::Tensor1dXf &query_bias,
    const Eigen::Tensor3dXf &key_weight, const Eigen::Tensor1dXf &key_bias,
    const Eigen::Tensor3dXf &query_decay_weight,
    const Eigen::Tensor1dXf &query_decay_bias,
    const Eigen::Tensor2dXf &query_decay_kernel,
    const Eigen::Tensor3dXf &proj_weight, const Eigen::Tensor1dXf &proj_bias,
    const int hidden_size)
{
    // local-attention block

    int B = x.dimension(0);
    int C = x.dimension(1);
    int T = x.dimension(2);

    const int num_heads = demucscpp_v3::LOCAL_ATTN_N_HEADS;

    // apply query conv1d on x
    Eigen::Tensor3dXf queries;
    Eigen::Tensor3dXf query_decays;
    Eigen::Tensor3dXf keys;
    Eigen::Tensor3dXf content;

    if (hidden_size == 192)
    {
        queries = demucscpp::conv1d<192, 192, 1, 1, 0, 1>(x, query_weight,
                                                          query_bias);
        keys = demucscpp::conv1d<192, 192, 1, 1, 0, 1>(x, key_weight, key_bias);
        query_decays = demucscpp::conv1d<192, 16, 1, 1, 0, 1>(
            x, query_decay_weight, query_decay_bias);
        content = demucscpp::conv1d<192, 192, 1, 1, 0, 1>(x, content_weight,
                                                          content_bias);
    }
    else
    {
        queries = demucscpp::conv1d<384, 384, 1, 1, 0, 1>(x, query_weight,
                                                          query_bias);
        keys = demucscpp::conv1d<384, 384, 1, 1, 0, 1>(x, key_weight, key_bias);
        query_decays = demucscpp::conv1d<384, 16, 1, 1, 0, 1>(
            x, query_decay_weight, query_decay_bias);
        content = demucscpp::conv1d<384, 384, 1, 1, 0, 1>(x, content_weight,
                                                          content_bias);
    }

    // so far, this is correct and matches pytorch

    int features_per_head = C / num_heads;

    // now implement dots calculation

    // Initialize the dots tensor
    Eigen::Tensor4dXf dots(B, num_heads, T, T);
    dots.setZero();

    // Precompute the square root of features_per_head
    float sqrt_features_per_head = std::sqrt(features_per_head);

    // apply a sigmoid activation with a 1/2 incorporated
    query_decays = query_decays.unaryExpr(
        [](float v) { return 0.5f / (1.0f + std::exp(-v)); });

    // Initialize the weights tensor for softmax
    Eigen::Tensor4dXf weights(B, num_heads, T, T);

    // Loop structure to compute both dot products and apply decay
    // simultaneously
    for (int b = 0; b < B; ++b)
    {
        for (int h = 0; h < num_heads; ++h)
        {
            for (int t = 0; t < T; ++t)
            {
                for (int s = 0; s < T; ++s)
                {
                    float dot_product = 0.0f;
                    float decay_effect = 0.0f;

                    // Compute the standard dot product
                    for (int c = 0; c < features_per_head; ++c)
                    {
                        int channel_index = h * features_per_head + c;
                        dot_product += queries(b, channel_index, s) *
                                       keys(b, channel_index, t);
                    }
                    dots(b, h, t, s) = dot_product / sqrt_features_per_head;

                    // Calculate decay effect for this dot product
                    for (int n = 0; n < LOCAL_ATTN_N_DECAY; ++n)
                    {
                        int decay_index = std::abs(
                            t - s); // Assuming decay_kernel is indexed by delta
                        float decay_kernel_value =
                            query_decay_kernel(n, decay_index);

                        // Transform query_decay by applying sigmoid directly
                        // here
                        float decay_query_value =
                            query_decays(b, h * LOCAL_ATTN_N_DECAY + n, s);

                        decay_effect += decay_kernel_value * decay_query_value;
                    }

                    // Apply decay effect directly to the dot product
                    if (t != s)
                    {
                        dots(b, h, t, s) += decay_effect;
                    }
                    else
                    {
                        dots(b, h, t, s) = -100.0f;
                    }
                }
            }

            for (int t = 0; t < T; ++t)
            {
                float max_val = -std::numeric_limits<float>::infinity();
                for (int s = 0; s < T; ++s)
                {
                    if (dots(b, h, s, t) > max_val)
                    {
                        max_val = dots(b, h, s, t);
                    }
                }

                float sum_exp = 0.0f;
                // Calculate the exponentials and sum them
                for (int s = 0; s < T; ++s)
                {
                    weights(b, h, s, t) = std::exp(dots(b, h, s, t) - max_val);
                    sum_exp += weights(b, h, s, t);
                }

                // Normalize the weights to form a proper probability
                // distribution
                for (int s = 0; s < T; ++s)
                {
                    weights(b, h, s, t) /= sum_exp;
                }
            }
        }
    }

    // Initialize the reshaped result tensor directly
    Eigen::Tensor3dXf reshaped_result(B, C, T);
    reshaped_result.setZero();

    // Merge computation of result tensor and reshaping
    for (int b = 0; b < B; ++b)
    {
        for (int h = 0; h < num_heads; ++h)
        {
            for (int c = 0; c < C / num_heads; ++c)
            {
                for (int s = 0; s < T; ++s)
                {
                    for (int t = 0; t < T; ++t)
                    {
                        // Directly update the reshaped_result tensor
                        int full_channel_index = h * (C / num_heads) + c;
                        reshaped_result(b, full_channel_index, s) +=
                            weights(b, h, t, s) *
                            content(b, h * (C / num_heads) + c, t);
                    }
                }
            }
        }
    }

    // Apply projection layer
    Eigen::Tensor3dXf projected_result;
    if (hidden_size == 192)
    {
        projected_result = demucscpp::conv1d<192, 192, 1, 1, 0, 1>(
            reshaped_result, proj_weight, proj_bias);
    }
    else
    {
        projected_result = demucscpp::conv1d<384, 384, 1, 1, 0, 1>(
            reshaped_result, proj_weight, proj_bias);
    }

    // Add x to projected_result
    x += projected_result;
}

void demucscpp_v3::apply_dconv_v3(
    const struct demucscpp_v3::demucs_v3_model &model, Eigen::Tensor3dXf &y,
    int freq_idx, int layer_idx, int mid_crop)
{
    // store another copy of y to sum back later
    Eigen::Tensor3dXf y_copy = y;

    // now dconv time

    switch (layer_idx)
    {
    case 0:
        y = demucscpp::conv1d<48, 12, 3, 1, 1, 1>(
            y, model.dconv_layers_0_conv1d_weight[freq_idx][layer_idx][0],
            model.dconv_layers_0_conv1d_bias[freq_idx][layer_idx][0]);
        break;
    case 1:
        y = demucscpp::conv1d<96, 24, 3, 1, 1, 1>(
            y, model.dconv_layers_0_conv1d_weight[freq_idx][layer_idx][0],
            model.dconv_layers_0_conv1d_bias[freq_idx][layer_idx][0]);
        break;
    case 2:
        y = demucscpp::conv1d<192, 48, 3, 1, 1, 1>(
            y, model.dconv_layers_0_conv1d_weight[freq_idx][layer_idx][0],
            model.dconv_layers_0_conv1d_bias[freq_idx][layer_idx][0]);
        break;
    case 3:
        y = demucscpp::conv1d<384, 96, 3, 1, 1, 1>(
            y, model.dconv_layers_0_conv1d_weight[freq_idx][layer_idx][0],
            model.dconv_layers_0_conv1d_bias[freq_idx][layer_idx][0]);
        break;
    };

    // y = demucscpp_v3::groupnorm::group_norm_fused_gelu(
    //     y,
    //     model.dconv_layers_1_groupnorm_weight[freq_idx][layer_idx]
    //                                          [0],
    //     model.dconv_layers_1_groupnorm_bias[freq_idx][layer_idx][0],
    //     1,
    //     1e-05);

    y = demucscpp::group_norm_fused_gelu(
        y, model.dconv_layers_1_groupnorm_weight[freq_idx][layer_idx][0],
        model.dconv_layers_1_groupnorm_bias[freq_idx][layer_idx][0], 1e-05);

    switch (layer_idx)
    {
    case 0:
        y = demucscpp::conv1d<12, 96, 1, 1, 0, 1>(
            y, model.dconv_layers_3_conv1d_weight[freq_idx][layer_idx][0],
            model.dconv_layers_3_conv1d_bias[freq_idx][layer_idx][0]);
        break;
    case 1:
        y = demucscpp::conv1d<24, 192, 1, 1, 0, 1>(
            y, model.dconv_layers_3_conv1d_weight[freq_idx][layer_idx][0],
            model.dconv_layers_3_conv1d_bias[freq_idx][layer_idx][0]);
        break;
    case 2:
        y = demucscpp::conv1d<48, 384, 1, 1, 0, 1>(
            y, model.dconv_layers_3_conv1d_weight[freq_idx][layer_idx][0],
            model.dconv_layers_3_conv1d_bias[freq_idx][layer_idx][0]);
        break;
    case 3:
        y = demucscpp::conv1d<96, 768, 1, 1, 0, 1>(
            y, model.dconv_layers_3_conv1d_weight[freq_idx][layer_idx][0],
            model.dconv_layers_3_conv1d_bias[freq_idx][layer_idx][0]);
        break;
    };

    y = demucscpp::group_norm(
        y, model.dconv_layers_4_groupnorm_weight[freq_idx][layer_idx][0],
        model.dconv_layers_4_groupnorm_bias[freq_idx][layer_idx][0], 1, 1e-05);

    y = demucscpp::glu(y, 1);

    y = demucscpp::layer_scale(
        y, model.dconv_layers_6_scale[freq_idx][layer_idx][0]);

    // now we add y to itself
    y = y + y_copy;

    // store another copy of y to sum back later
    y_copy = y;

    // NEXT ENTIRE SUBSEQUENCE OF DCONV WITH SLIGHTLY DIFFERENT PARAMS

    // Conv1d(48, 6, kernel_size=(3,), stride=(1,), padding=(2,), dilation=(2,))
    switch (layer_idx)
    {
    case 0:
        y = demucscpp::conv1d<48, 12, 3, 1, 2, 2>(
            y, model.dconv_layers_0_conv1d_weight[freq_idx][layer_idx][1],
            model.dconv_layers_0_conv1d_bias[freq_idx][layer_idx][1]);
        break;
    case 1:
        y = demucscpp::conv1d<96, 24, 3, 1, 2, 2>(
            y, model.dconv_layers_0_conv1d_weight[freq_idx][layer_idx][1],
            model.dconv_layers_0_conv1d_bias[freq_idx][layer_idx][1]);
        break;
    case 2:
        y = demucscpp::conv1d<192, 48, 3, 1, 2, 2>(
            y, model.dconv_layers_0_conv1d_weight[freq_idx][layer_idx][1],
            model.dconv_layers_0_conv1d_bias[freq_idx][layer_idx][1]);
        break;
    case 3:
        y = demucscpp::conv1d<384, 96, 3, 1, 2, 2>(
            y, model.dconv_layers_0_conv1d_weight[freq_idx][layer_idx][1],
            model.dconv_layers_0_conv1d_bias[freq_idx][layer_idx][1]);
        break;
    };

    Eigen::Tensor3dXf y_cropped =
        y.slice(Eigen::array<Eigen::Index, 3>({0, 0, 0}),
                Eigen::array<Eigen::Index, 3>(
                    {y.dimension(0), y.dimension(1), mid_crop}));

    y = y_cropped;

    y = demucscpp::group_norm_fused_gelu(
        y, model.dconv_layers_1_groupnorm_weight[freq_idx][layer_idx][1],
        model.dconv_layers_1_groupnorm_bias[freq_idx][layer_idx][1], 1e-05);

    // Conv1d(6, 96, kernel_size=(1,), stride=(1,))
    switch (layer_idx)
    {
    case 0:
        y = demucscpp::conv1d<12, 96, 1, 1, 0, 1>(
            y, model.dconv_layers_3_conv1d_weight[freq_idx][layer_idx][1],
            model.dconv_layers_3_conv1d_bias[freq_idx][layer_idx][1]);
        break;
    case 1:
        y = demucscpp::conv1d<24, 192, 1, 1, 0, 1>(
            y, model.dconv_layers_3_conv1d_weight[freq_idx][layer_idx][1],
            model.dconv_layers_3_conv1d_bias[freq_idx][layer_idx][1]);
        break;
    case 2:
        y = demucscpp::conv1d<48, 384, 1, 1, 0, 1>(
            y, model.dconv_layers_3_conv1d_weight[freq_idx][layer_idx][1],
            model.dconv_layers_3_conv1d_bias[freq_idx][layer_idx][1]);
        break;
    case 3:
        y = demucscpp::conv1d<96, 768, 1, 1, 0, 1>(
            y, model.dconv_layers_3_conv1d_weight[freq_idx][layer_idx][1],
            model.dconv_layers_3_conv1d_bias[freq_idx][layer_idx][1]);
        break;
    };

    y = demucscpp::group_norm(
        y, model.dconv_layers_4_groupnorm_weight[freq_idx][layer_idx][1],
        model.dconv_layers_4_groupnorm_bias[freq_idx][layer_idx][1], 1, 1e-05);

    y = demucscpp::glu(y, 1);
    y = demucscpp::layer_scale(
        y, model.dconv_layers_6_scale[freq_idx][layer_idx][1]);

    // if y_copy is shorter than y in the last dim
    // pad the last dim with zeros to match

    if (y_copy.dimension(2) < y.dimension(2))
    {
        // pad the last dim with zeros to match
        Eigen::Tensor3dXf padded_tensor_copy(
            y_copy.dimension(0), y_copy.dimension(1), y.dimension(2));
        padded_tensor_copy.setZero();
        padded_tensor_copy.slice(Eigen::array<Eigen::Index, 3>({0, 0, 0}),
                                 y_copy.dimensions()) = y_copy;
        y_copy = padded_tensor_copy;
    }

    // now sum with itself
    y = y + y_copy;
}

void demucscpp_v3::apply_dconv_v3_encoder_4_5(
    const struct demucscpp_v3::demucs_v3_model &model, Eigen::Tensor3dXf &y,
    int encoder_idx, int mid_crop,
    struct demucscpp_v3::demucs_v3_segment_buffers &buffers)
{
    int lstm_hidden_size = encoder_idx == 0 ? demucscpp_v3::LSTM_HIDDEN_SIZE_0
                                            : demucscpp_v3::LSTM_HIDDEN_SIZE_1;

    // store another copy of y to sum back later
    Eigen::Tensor3dXf y_copy = y;

    // now dconv time

    switch (encoder_idx)
    {
    case 0:
        y = demucscpp::conv1d<768, 192, 3, 1, 1, 1>(
            y, model.encoder_4_5_dconv_layers_0_conv1d_weight[encoder_idx][0],
            model.encoder_4_5_dconv_layers_0_conv1d_bias[encoder_idx][0]);
        break;
    case 1:
        y = demucscpp::conv1d<1536, 384, 3, 1, 1, 1>(
            y, model.encoder_4_5_dconv_layers_0_conv1d_weight[encoder_idx][0],
            model.encoder_4_5_dconv_layers_0_conv1d_bias[encoder_idx][0]);
        break;
    };

    y = demucscpp::group_norm_fused_gelu(
        y, model.encoder_4_5_dconv_layers_1_groupnorm_weight[encoder_idx][0],
        model.encoder_4_5_dconv_layers_1_groupnorm_bias[encoder_idx][0], 1e-05);

    // transpose it to put time seq last
    Eigen::MatrixXf y_mat =
        Eigen::Map<Eigen::MatrixXf>(y.data(), y.dimension(1), y.dimension(2))
            .transpose();

    // then, bilstm
    demucscpp_v3::lstm_forward(model, encoder_idx, 0, y_mat, buffers,
                               lstm_hidden_size);

    // access last element of the last dim which is the output of the bilstm
    Eigen::MatrixXf lstm_out_0 = buffers.lstm_output[encoder_idx][0][1];

    // set lstm state to 0
    demucscpp_v3::lstm_reset_zero(encoder_idx, 0, buffers);

    // apply the linear layer on the lstm_out_0
    lstm_out_0 = (lstm_out_0 *
                  model.encoder_4_5_dconv_layers_3_linear_weight[encoder_idx][0]
                      .transpose())
                     .rowwise() +
                 model.encoder_4_5_dconv_layers_3_linear_bias[encoder_idx][0]
                     .transpose();

    // then apply skip connection
    lstm_out_0 = lstm_out_0 + y_mat;

    // copy it to a original 3d tensor
    y = Eigen::TensorMap<Eigen::Tensor3dXf>(
        lstm_out_0.data(), lstm_out_0.rows(), 1, lstm_out_0.cols());

    // swap dims from 0,1,2 to 1,2,0
    Eigen::Tensor3dXf y_shuff = y.shuffle(Eigen::array<int, 3>({1, 2, 0}));

    // then, localattn
    demucscpp_v3::local_attention(
        y_shuff,
        model.encoder_4_5_dconv_layers_4_content_weight[encoder_idx][0],
        model.encoder_4_5_dconv_layers_4_content_bias[encoder_idx][0],
        model.encoder_4_5_dconv_layers_4_query_weight[encoder_idx][0],
        model.encoder_4_5_dconv_layers_4_query_bias[encoder_idx][0],
        model.encoder_4_5_dconv_layers_4_key_weight[encoder_idx][0],
        model.encoder_4_5_dconv_layers_4_key_bias[encoder_idx][0],
        model.encoder_4_5_dconv_layers_4_query_decay_weight[encoder_idx][0],
        model.encoder_4_5_dconv_layers_4_query_decay_bias[encoder_idx][0],
        buffers.local_attn_decay_kernel,
        model.encoder_4_5_dconv_layers_4_proj_weight[encoder_idx][0],
        model.encoder_4_5_dconv_layers_4_proj_bias[encoder_idx][0],
        lstm_hidden_size);

    y = y_shuff;

    switch (encoder_idx)
    {
    case 0:
        y = demucscpp::conv1d<192, 1536, 1, 1, 0, 1>(
            y, model.encoder_4_5_dconv_layers_5_conv1d_weight[encoder_idx][0],
            model.encoder_4_5_dconv_layers_5_conv1d_bias[encoder_idx][0]);
        break;
    case 1:
        y = demucscpp::conv1d<384, 3072, 1, 1, 0, 1>(
            y, model.encoder_4_5_dconv_layers_5_conv1d_weight[encoder_idx][0],
            model.encoder_4_5_dconv_layers_5_conv1d_bias[encoder_idx][0]);
        break;
    };

    y = demucscpp::group_norm(
        y, model.encoder_4_5_dconv_layers_6_groupnorm_weight[encoder_idx][0],
        model.encoder_4_5_dconv_layers_6_groupnorm_bias[encoder_idx][0], 1,
        1e-05);

    y = demucscpp::glu(y, 1);

    y = demucscpp::layer_scale(
        y, model.encoder_4_5_dconv_layers_8_scale[encoder_idx][0]);

    // now we add y to itself
    y = y + y_copy;

    // store another copy of y to sum back later
    y_copy = y;

    // NEXT ENTIRE SUBSEQUENCE OF DCONV WITH SLIGHTLY DIFFERENT PARAMS
    // now dconv time

    switch (encoder_idx)
    {
    case 0:
        y = demucscpp::conv1d<768, 192, 3, 1, 2, 2>(
            y, model.encoder_4_5_dconv_layers_0_conv1d_weight[encoder_idx][1],
            model.encoder_4_5_dconv_layers_0_conv1d_bias[encoder_idx][1]);
        break;
    case 1:
        y = demucscpp::conv1d<1536, 384, 3, 1, 2, 2>(
            y, model.encoder_4_5_dconv_layers_0_conv1d_weight[encoder_idx][1],
            model.encoder_4_5_dconv_layers_0_conv1d_bias[encoder_idx][1]);
        break;
    };

    Eigen::Tensor3dXf y_cropped =
        y.slice(Eigen::array<Eigen::Index, 3>({0, 0, 0}),
                Eigen::array<Eigen::Index, 3>(
                    {y.dimension(0), y.dimension(1), mid_crop}));

    y = y_cropped;

    y = demucscpp::group_norm_fused_gelu(
        y, model.encoder_4_5_dconv_layers_1_groupnorm_weight[encoder_idx][1],
        model.encoder_4_5_dconv_layers_1_groupnorm_bias[encoder_idx][1], 1e-05);

    // transpose it to put time seq last
    y_mat =
        Eigen::Map<Eigen::MatrixXf>(y.data(), y.dimension(1), y.dimension(2))
            .transpose();

    // then, bilstm
    demucscpp_v3::lstm_forward(model, encoder_idx, 1, y_mat, buffers,
                               lstm_hidden_size);

    // access last element of the last dim which is the output of the bilstm
    lstm_out_0 = buffers.lstm_output[encoder_idx][1][1];

    // reset lstm state to 0
    demucscpp_v3::lstm_reset_zero(encoder_idx, 1, buffers);

    // apply the linear layer on the lstm_out_0
    lstm_out_0 = (lstm_out_0 *
                  model.encoder_4_5_dconv_layers_3_linear_weight[encoder_idx][1]
                      .transpose())
                     .rowwise() +
                 model.encoder_4_5_dconv_layers_3_linear_bias[encoder_idx][1]
                     .transpose();

    // then apply skip connection
    lstm_out_0 = lstm_out_0 + y_mat;

    // copy it to a original 3d tensor
    y = Eigen::TensorMap<Eigen::Tensor3dXf>(
        lstm_out_0.data(), lstm_out_0.rows(), 1, lstm_out_0.cols());

    // swap dims from 0,1,2 to 1,2,0
    y_shuff = y.shuffle(Eigen::array<int, 3>({1, 2, 0}));

    // then, localattn
    demucscpp_v3::local_attention(
        y_shuff,
        model.encoder_4_5_dconv_layers_4_content_weight[encoder_idx][1],
        model.encoder_4_5_dconv_layers_4_content_bias[encoder_idx][1],
        model.encoder_4_5_dconv_layers_4_query_weight[encoder_idx][1],
        model.encoder_4_5_dconv_layers_4_query_bias[encoder_idx][1],
        model.encoder_4_5_dconv_layers_4_key_weight[encoder_idx][1],
        model.encoder_4_5_dconv_layers_4_key_bias[encoder_idx][1],
        model.encoder_4_5_dconv_layers_4_query_decay_weight[encoder_idx][1],
        model.encoder_4_5_dconv_layers_4_query_decay_bias[encoder_idx][1],
        buffers.local_attn_decay_kernel,
        model.encoder_4_5_dconv_layers_4_proj_weight[encoder_idx][1],
        model.encoder_4_5_dconv_layers_4_proj_bias[encoder_idx][1],
        lstm_hidden_size);

    y = y_shuff;

    switch (encoder_idx)
    {
    case 0:
        y = demucscpp::conv1d<192, 1536, 1, 1, 0, 1>(
            y, model.encoder_4_5_dconv_layers_5_conv1d_weight[encoder_idx][1],
            model.encoder_4_5_dconv_layers_5_conv1d_bias[encoder_idx][1]);
        break;
    case 1:
        y = demucscpp::conv1d<384, 3072, 1, 1, 0, 1>(
            y, model.encoder_4_5_dconv_layers_5_conv1d_weight[encoder_idx][1],
            model.encoder_4_5_dconv_layers_5_conv1d_bias[encoder_idx][1]);
        break;
    };

    y = demucscpp::group_norm(
        y, model.encoder_4_5_dconv_layers_6_groupnorm_weight[encoder_idx][1],
        model.encoder_4_5_dconv_layers_6_groupnorm_bias[encoder_idx][1], 1,
        1e-05);

    y = demucscpp::glu(y, 1);

    y = demucscpp::layer_scale(
        y, model.encoder_4_5_dconv_layers_8_scale[encoder_idx][1]);

    // now sum with itself
    y = y + y_copy;
}
