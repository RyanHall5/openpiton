// Runs one ORIGINAL AIBench Micro Pthreads kernel on x86 with the same input as
// the bare-metal port and prints the output checksum, to compare with the
// port's "Checksum:" line. Built by check_ports.sh, once per kernel:
//   g++ -std=c++11 -O0 -pthread -DK_<kernel> -Ihost -I<AIBench>/Pthread/Pthread aib_host.cpp
//   ./a.out BATCH IMG_H IMG_W THREADS
// The kernel code itself is AIBench's, unchanged: its header (and for
// batch_norm/max_pool/avg_pool its .cpp, minus main()) is included as is.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <vector>
#include <mutex>

#if defined(K_relu)
#include "relu.h"
#elif defined(K_sigmoid)
#include "sigmoid.h"
#elif defined(K_tanh)
#include "tanh.h"
#elif defined(K_softmax)
#include "softmax.h"
#elif defined(K_dropout)
// dropout.h calls srand(time(NULL)); the port uses a fixed time instead
#define time(x) ((time_t)1763856000u)
#include "dropout.h"
#undef time
#elif defined(K_add)
#include "add.h"
#elif defined(K_multiply)
#include "multiply.h"
#elif defined(K_matmul)
#include "matmul.h"
#elif defined(K_batch_norm)
#include "batch_norm.h"
#undef BATCH_NORM_TEST
#include "batch_norm.cpp"
#elif defined(K_max_pool)
#include "max_pool.h"
#undef MAX_POOL_TEST
#include "max_pool.cpp"
#elif defined(K_avg_pool)
#include "avg_pool.h"
#undef AVG_POOL_TEST
#include "avg_pool.cpp"
#else
#error define K_<kernel>
#endif

// same generator and checksum as rit/aibench.h
static uint32_t lcg_seed = 12345;
static float next_pixel() { lcg_seed = lcg_seed * 1103515245u + 12345u; return (float)((lcg_seed >> 16) & 0xff); }
static uint64_t checksum(const float *p, long n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (long i = 0; i < n; i++) {
        uint32_t u; memcpy(&u, &p[i], 4);
        if (p[i] != p[i]) u = 0x7fc00000u;
        for (int b = 0; b < 4; b++) { h ^= (u >> (8 * b)) & 0xff; h *= 0x100000001b3ull; }
    }
    return h;
}
static uint64_t checksum4(vector<vector<vector<vector<float>>>> &v)
{
    vector<float> flat;
    for (auto &a : v) for (auto &b : a) for (auto &c : b) for (float x : c) flat.push_back(x);
    return checksum(flat.data(), flat.size());
}

int main(int argc, char **argv)
{
    int N = atoi(argv[1]), H = atoi(argv[2]), W = atoi(argv[3]), T = atoi(argv[4]);
    int C = 3;
    uint64_t sum;
#if defined(K_max_pool) || defined(K_avg_pool)
    // NCHW images, as run-pthread.sh runs the pooling kernels
    batchImage input(N, singleImage(C, vector<vector<float>>(H, vector<float>(W))));
    for (int n = 0; n < N; n++) for (int c = 0; c < C; c++) for (int h = 0; h < H; h++) for (int w = 0; w < W; w++)
        input[n][c][h][w] = next_pixel();
    int k[] = {1, 1, 3, 3}; vector<int> ksize(k, k + 4);
    int s[] = {1, 1, 2, 2}; vector<int> strides(s, s + 4);
#if defined(K_max_pool)
    max_pool op(input, ksize, strides, "SAME", "NCHW");
#else
    avg_pool op(input, ksize, strides, "SAME", "NCHW");
#endif
    sum = checksum4(*op.run(T));
#else
    // NHWC images, as the tensor kernels and batch_norm load them
    batchImage input(N, singleImage(H, vector<vector<float>>(W, vector<float>(C))));
    for (int n = 0; n < N; n++) for (int h = 0; h < H; h++) for (int w = 0; w < W; w++) for (int c = 0; c < C; c++)
        input[n][h][w][c] = next_pixel();
#if defined(K_batch_norm)
    // thread_Batch_norm(), keeping the output
    vector<vector<vector<float>>> mean_tmp(N, vector<vector<float>>(H, vector<float>(W, 0.0)));
    vector<vector<vector<float>>> variance_tmp(N, vector<vector<float>>(H, vector<float>(W, 1)));
    batch_norm op(input, mean_tmp, variance_tmp, 1, 1, 1);
    sum = checksum4(*op.run(T));
#else
    // the kernel's threadBatch(), keeping the output
    vector<float> data;
    fourDim2oneDim(input, data);
#if defined(K_matmul)
    vector<int> shape1 = { N * H, W * C }, shape2 = { W * C, N * H };
    tensor<float> tensor1(2, shape1, data), tensor2(2, shape2, data);
    matmul<float> op(&tensor1, &tensor2);
#else
    vector<int> shape = { N, H, W, C };
#if defined(K_add) || defined(K_multiply)
    tensor<float> tensor1(4, shape, data), tensor2(4, shape, data);
#if defined(K_add)
    addOperator<float> op(&tensor1, &tensor2);
#else
    multiplyOperator<float> op(&tensor1, &tensor2);
#endif
#else
    tensor<float> tensor1(4, shape, data);
#if defined(K_relu)
    relu<float> op(&tensor1);
#elif defined(K_sigmoid)
    sigmoid<float> op(&tensor1);
#elif defined(K_tanh)
    tf_tanh<float> op(&tensor1);
#elif defined(K_softmax)
    softmax<float> op(&tensor1);
#elif defined(K_dropout)
    dropout<float> op(&tensor1, 0.4);
#endif
#endif
#endif
    tensor<float> *out = op.run(T);
    sum = checksum(out->get_data()->data(), out->get_data()->size());
#endif
#endif
    printf("Checksum: 0x%016llx\n", (unsigned long long)sum);
    return 0;
}
