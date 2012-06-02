#ifndef VEXCL_VEXCL_HPP
#define VEXCL_VEXCL_HPP

/*
The MIT License

Copyright (c) 2012 Denis Demidov <ddemidov@ksu.ru>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

/**
 * \file   vexcl.hpp
 * \author Denis Demidov <ddemidov@ksu.ru>
 * \brief  Vector expression template library for OpenCL.
 */

/**
\mainpage VexCL

VexCL is vector expression template library for OpenCL. It has been created for
ease of C++ based OpenCL development.  Multi-device (and multi-platform)
computations are supported. The source code is available at
https://github.com/ddemidov/vexcl.

\section devlist Selection of compute devices

You can select any number of available compute devices, which satisfy provided
filters. Filter is a functor returning bool and acting on a cl::Device
parameter. Several standard filters are provided, such as device type or name
filter, double precision support etc. Filters can be combined with logical
operators. In the example below all devices with names matching "Radeon" and
supporting double precision are selected:
\code
#include <iostream>
#include <vexcl/vexcl.hpp>
using namespace vex;
int main() {
    auto device = device_list(
        Filter::Name("Radeon") && Filter::DoublePrecision()
        );
    std::cout << device << std::endl;
}
\endcode

Often you want not just device list, but initialized OpenCL context with
command queue on each available device. This may be achieved with queue_list()
function:
\code
cl::Context context;
std::vector<cl::CommandQueue> queue;
// Select no more than 2 NVIDIA GPUs:
std::tie(context, queue) = queue_list(
    [](const cl::Device &d) {
        return d.getInfo<CL_DEVICE_VENDOR>() == "NVIDIA Corporation";
    } && Filter::Count(2)
    );
\endcode

\section vector Memory allocation and vector arithmetic

Once you got queue list, you can allocate OpenCL buffers on the associated
devices. vex::vector constructor accepts std::vector of cl::CommandQueue.
The contents of the created vector will be equally partitioned between each
queue (presumably, each of the provided queues is linked with separate device). 
Size of each partition will be proportional to relative device bandwidth unless
macro VEXCL_DUMB_PARTITIONING is defined, in which case equal partitioning
scheme will be applied. Device bandwidth is measured first time it is requested
by launch of small test kernel.

Multi-platform computation is supported (that is, you can spread your vectors
across devices by different vendors), but should be used with caution: all
computations will be performed with the speed of the slowest device selected.

In the example below host vector is allocated and initialized, then copied to
all devices obtained with the queue_list() call. A couple of empty device
vectors are allocated as well:
\code
const uint n = 1 << 20;
std::vector<double> x(n);
std::generate(x.begin(), x.end(), [](){ return (double)rand() / RAND_MAX; });

cl::Context context;
std::vector<cl::CommandQueue> queue;
std::tie(context, queue) = queue_list(Filter::Type(CL_DEVICE_TYPE_GPU));

vex::vector<double> X(queue, CL_MEM_READ_ONLY,  x);
vex::vector<double> Y(queue, CL_MEM_READ_WRITE, n);
vex::vector<double> Z(queue, CL_MEM_READ_WRITE, n);
\endcode

You can now use simple vector arithmetic with device vector. For every
expression you use, appropriate kernel is compiled (first time it is
encountered in your program) and called automagically.

Vectors are processed in parallel across all devices they were allocated on:
\code
Y = 42;
Z = sqrt(2 * X) + cos(Y);
\endcode

You can copy the result back to host or you can use vector::operator[] to
read (or write) vector elements directly. Though latter technique is very
ineffective and should be used for debugging purposes only.
\code
copy(Z, x);
assert(x[42] == Z[42]);
\endcode

Another frequently performed operation is reduction of a vector expression to
single value, such as summation. This can be done with vex::Reductor class:
\code
Reductor<double> sum(queue);

std::cout << sum(Z) << std::endl;
std::cout << sum(sqrt(2 * X) + cos(Y)) << std::endl;
\endcode

\section spmv Sparse matrix-vector multiplication

One of the most common operations in linear algebra is matrix-vector
multiplication. Class vex::SpMat holds representation of a sparse matrix,
spanning several devices. In the example below it is used for solution of a
system of linear equations with conjugate gradients method:
\code
typedef double real;
// Solve system of linear equations A u = f with conjugate gradients method.
// Input matrix is represented in CSR format (parameters row, col, and val).
void cg_gpu(
	const std::vector<uint> &row,	// Indices to col and val vectors.
	const std::vector<uint> &col,	// Column numbers of non-zero elements.
	const std::vector<real> &val,	// Values of non-zero elements.
	const std::vector<real> &rhs,	// Right-hand side.
	std::vector<real> &x		// In: initial approximation; out: result.
	)
{
    // Init OpenCL.
    cl::Context context;
    std::vector<cl::CommandQueue> queue;

    std::tie(context, queue) = queue_list(Filter::Type(CL_DEVICE_TYPE_GPU));

    // Move data to compute devices.
    uint n = x.size();
    vex::SpMat<real>  A(queue, n, row.data(), col.data(), val.data());
    vex::vector<real> f(queue, CL_MEM_READ_ONLY,  rhs);
    vex::vector<real> u(queue, CL_MEM_READ_WRITE, x);
    vex::vector<real> r(queue, CL_MEM_READ_WRITE, n);
    vex::vector<real> p(queue, CL_MEM_READ_WRITE, n);
    vex::vector<real> q(queue, CL_MEM_READ_WRITE, n);

    Reductor<real,MAX> max(queue);
    Reductor<real,SUM> sum(queue);

    // Solve equation Au = f with conjugate gradients method.
    real rho1, rho2;
    r = f - A * u;

    for(uint iter = 0; max(Abs(r)) > 1e-8 && iter < n; iter++) {
	rho1 = sum(r * r);

	if (iter == 0) {
	    p = r;
	} else {
	    real beta = rho1 / rho2;
	    p = r + beta * p;
	}

	q = A * p;

	real alpha = rho1 / sum(p * q);

	u += alpha * p;
	r -= alpha * q;

	rho2 = rho1;
    }

    // Get result to host.
    copy(u, x);
}
\endcode

\section custkern Using custom kernels

Custom kernels are of course possible as well. vector::operator(uint)
returns cl::Buffer object for a specified device:
\code
cl::Context context;
std::vector<cl::CommandQueue> queue;
std::tie(context, queue) = queue_list(Filter::Type(CL_DEVICE_TYPE_GPU));

const uint n = 1 << 20;
vex::vector<float> x(queue, CL_MEM_WRITE_ONLY, n);

auto program = build_sources(context, std::string(
    "kernel void dummy(uint size, global float *x)\n"
    "{\n"
    "    uint i = get_global_id(0);\n"
    "    if (i < size) x[i] = 4.2;\n"
    "}\n"
    ));

for(uint d = 0; d < queue.size(); d++) {
    auto dummy = cl::Kernel(program, "dummy").bind(queue[d], alignup(n, 256), 256);
    dummy((uint)x.part_size(d), x(d));
}

Reductor<float,SUM> sum(queue);
std::cout << sum(x) << std::endl;
\endcode

\section scalability Scalability

In the images below, scalability of the library with respect to number of
compute devices is shown. Effective performance (GFLOPS) and bandwidth (GB/sec)
were measured by launching big number of test kernels on one, two, or three
Nvidia Tesla C2070 cards. The results shown are averaged over 20 runs.

The details of the experiments may be found in <a
href="https://github.com/ddemidov/vexcl/blob/master/examples/profiling.cpp">examples/profiling.cpp</a>
file.  Basically, performance of the following code was measured:

\code
// Vector arithmetic
a += b + c * d;

// Reduction
double s = sum(a * b);

// SpMV
y += A * x;
\endcode

\image html perf.png ""

*/

#ifdef WIN32
#  pragma warning(disable : 4290)
#  define NOMINMAX
#endif

#define __CL_ENABLE_EXCEPTIONS

#include <CL/cl.hpp>
#include <iostream>

#include <vexcl/util.hpp>
#include <vexcl/devlist.hpp>
#include <vexcl/vector.hpp>
#include <vexcl/spmat.hpp>
#include <vexcl/reduce.hpp>
#include <vexcl/profiler.hpp>

#endif