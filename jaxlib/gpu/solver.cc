/* Copyright 2019 The JAX Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_format.h"
#include "jaxlib/gpu/gpu_kernel_helpers.h"
#include "jaxlib/gpu/solver_kernels.h"
#include "jaxlib/gpu/vendor.h"
#include "jaxlib/kernel_pybind11_helpers.h"
#include "include/pybind11/numpy.h"
#include "include/pybind11/pybind11.h"
#include "include/pybind11/stl.h"

namespace jax {
namespace JAX_GPU_NAMESPACE {
namespace {
namespace py = pybind11;

// Converts a NumPy dtype to a Type.
SolverType DtypeToSolverType(const py::dtype& np_type) {
  static auto* types =
      new absl::flat_hash_map<std::pair<char, int>, SolverType>({
          {{'f', 4}, SolverType::F32},
          {{'f', 8}, SolverType::F64},
          {{'c', 8}, SolverType::C64},
          {{'c', 16}, SolverType::C128},
      });
  auto it = types->find({np_type.kind(), np_type.itemsize()});
  if (it == types->end()) {
    throw std::invalid_argument(
        absl::StrFormat("Unsupported dtype %s", py::repr(np_type)));
  }
  return it->second;
}

// potrf: Cholesky decomposition

// Returns the workspace size and a descriptor for a potrf operation.
std::pair<int, py::bytes> BuildPotrfDescriptor(const py::dtype& dtype,
                                               bool lower, int b, int n) {
  SolverType type = DtypeToSolverType(dtype);
  auto h = SolverHandlePool::Borrow();
  JAX_THROW_IF_ERROR(h.status());
  auto& handle = *h;
  int lwork;
  std::int64_t workspace_size;
  gpusolverFillMode_t uplo =
      lower ? GPUSOLVER_FILL_MODE_LOWER : GPUSOLVER_FILL_MODE_UPPER;
  if (b == 1) {
    switch (type) {
      case SolverType::F32:
        JAX_THROW_IF_ERROR(
            JAX_AS_STATUS(gpusolverDnSpotrf_bufferSize(handle.get(), uplo, n,
                                                       /*A=*/nullptr,
                                                       /*lda=*/n, &lwork)));
        workspace_size = lwork * sizeof(float);
        break;
      case SolverType::F64:
        JAX_THROW_IF_ERROR(
            JAX_AS_STATUS(gpusolverDnDpotrf_bufferSize(handle.get(), uplo, n,
                                                       /*A=*/nullptr,
                                                       /*lda=*/n, &lwork)));
        workspace_size = lwork * sizeof(double);
        break;
      case SolverType::C64:
        JAX_THROW_IF_ERROR(
            JAX_AS_STATUS(gpusolverDnCpotrf_bufferSize(handle.get(), uplo, n,
                                                       /*A=*/nullptr,
                                                       /*lda=*/n, &lwork)));
        workspace_size = lwork * sizeof(gpuComplex);
        break;
      case SolverType::C128:
        JAX_THROW_IF_ERROR(
            JAX_AS_STATUS(gpusolverDnZpotrf_bufferSize(handle.get(), uplo, n,
                                                       /*A=*/nullptr,
                                                       /*lda=*/n, &lwork)));
        workspace_size = lwork * sizeof(gpuDoubleComplex);
        break;
    }
  } else {
#ifdef JAX_GPU_CUDA
    // We use the workspace buffer for our own scratch space.
    workspace_size = sizeof(void*) * b;
#else
    // TODO(rocm): when cuda and hip had same API for batched potrf, remove this
    // batched potrf has different API compared to CUDA. In hip we still need to
    // create the workspace and additional space to copy the batch array
    // pointers
    switch (type) {
      case SolverType::F32:
        JAX_THROW_IF_ERROR(JAX_AS_STATUS(
            hipsolverSpotrfBatched_bufferSize(handle.get(), uplo, n,
                                              /*A=*/nullptr,
                                              /*lda=*/n, &lwork, b)));
        workspace_size = (lwork * sizeof(float)) + (b * sizeof(float*));
        break;
      case SolverType::F64:
        JAX_THROW_IF_ERROR(JAX_AS_STATUS(
            hipsolverDpotrfBatched_bufferSize(handle.get(), uplo, n,
                                              /*A=*/nullptr,
                                              /*lda=*/n, &lwork, b)));
        workspace_size = (lwork * sizeof(double)) + (b * sizeof(double*));
        break;
      case SolverType::C64:
        JAX_THROW_IF_ERROR(JAX_AS_STATUS(
            hipsolverCpotrfBatched_bufferSize(handle.get(), uplo, n,
                                              /*A=*/nullptr,
                                              /*lda=*/n, &lwork, b)));
        workspace_size =
            (lwork * sizeof(hipComplex)) + (b * sizeof(hipComplex*));
        break;
      case SolverType::C128:
        JAX_THROW_IF_ERROR(JAX_AS_STATUS(
            hipsolverZpotrfBatched_bufferSize(handle.get(), uplo, n,
                                              /*A=*/nullptr,
                                              /*lda=*/n, &lwork, b)));
        workspace_size = (lwork * sizeof(hipDoubleComplex)) +
                         (b * sizeof(hipDoubleComplex*));
        break;
    }
#endif  // JAX_GPU_CUDA
  }
  return {workspace_size,
          PackDescriptor(PotrfDescriptor{type, uplo, b, n, lwork})};
}

// getrf: LU decomposition

// Returns the workspace size and a descriptor for a getrf operation.
std::pair<int, py::bytes> BuildGetrfDescriptor(const py::dtype& dtype, int b,
                                               int m, int n) {
  SolverType type = DtypeToSolverType(dtype);
  auto h = SolverHandlePool::Borrow();
  JAX_THROW_IF_ERROR(h.status());
  auto& handle = *h;
  int lwork;
  switch (type) {
    case SolverType::F32:
      JAX_THROW_IF_ERROR(
          JAX_AS_STATUS(gpusolverDnSgetrf_bufferSize(handle.get(), m, n,
                                                     /*A=*/nullptr,
                                                     /*lda=*/m, &lwork)));
      break;
    case SolverType::F64:
      JAX_THROW_IF_ERROR(
          JAX_AS_STATUS(gpusolverDnDgetrf_bufferSize(handle.get(), m, n,
                                                     /*A=*/nullptr,
                                                     /*lda=*/m, &lwork)));
      break;
    case SolverType::C64:
      JAX_THROW_IF_ERROR(
          JAX_AS_STATUS(gpusolverDnCgetrf_bufferSize(handle.get(), m, n,
                                                     /*A=*/nullptr,
                                                     /*lda=*/m, &lwork)));
      break;
    case SolverType::C128:
      JAX_THROW_IF_ERROR(
          JAX_AS_STATUS(gpusolverDnZgetrf_bufferSize(handle.get(), m, n,
                                                     /*A=*/nullptr,
                                                     /*lda=*/m, &lwork)));
      break;
  }
  return {lwork, PackDescriptor(GetrfDescriptor{type, b, m, n, lwork})};
}

// geqrf: QR decomposition

// Returns the workspace size and a descriptor for a geqrf operation.
std::pair<int, py::bytes> BuildGeqrfDescriptor(const py::dtype& dtype, int b,
                                               int m, int n) {
  SolverType type = DtypeToSolverType(dtype);
  auto h = SolverHandlePool::Borrow();
  JAX_THROW_IF_ERROR(h.status());
  auto& handle = *h;
  int lwork;
  switch (type) {
    case SolverType::F32:
      JAX_THROW_IF_ERROR(
          JAX_AS_STATUS(gpusolverDnSgeqrf_bufferSize(handle.get(), m, n,
                                                     /*A=*/nullptr,
                                                     /*lda=*/m, &lwork)));
      break;
    case SolverType::F64:
      JAX_THROW_IF_ERROR(
          JAX_AS_STATUS(gpusolverDnDgeqrf_bufferSize(handle.get(), m, n,
                                                     /*A=*/nullptr,
                                                     /*lda=*/m, &lwork)));
      break;
    case SolverType::C64:
      JAX_THROW_IF_ERROR(
          JAX_AS_STATUS(gpusolverDnCgeqrf_bufferSize(handle.get(), m, n,
                                                     /*A=*/nullptr,
                                                     /*lda=*/m, &lwork)));
      break;
    case SolverType::C128:
      JAX_THROW_IF_ERROR(
          JAX_AS_STATUS(gpusolverDnZgeqrf_bufferSize(handle.get(), m, n,
                                                     /*A=*/nullptr,
                                                     /*lda=*/m, &lwork)));
      break;
  }
  return {lwork, PackDescriptor(GeqrfDescriptor{type, b, m, n, lwork})};
}

#ifdef JAX_GPU_CUDA

// csrlsvqr: Linear system solve via Sparse QR

// Returns a descriptor for a csrlsvqr operation.
py::bytes BuildCsrlsvqrDescriptor(const py::dtype& dtype, int n, int nnzA,
                                  int reorder, double tol) {
  SolverType type = DtypeToSolverType(dtype);
  return PackDescriptor(CsrlsvqrDescriptor{type, n, nnzA, reorder, tol});
}

#endif  // JAX_GPU_CUDA

// orgqr/ungqr: apply elementary Householder transformations

// Returns the workspace size and a descriptor for a geqrf operation.
std::pair<int, py::bytes> BuildOrgqrDescriptor(const py::dtype& dtype, int b,
                                               int m, int n, int k) {
  SolverType type = DtypeToSolverType(dtype);
  auto h = SolverHandlePool::Borrow();
  JAX_THROW_IF_ERROR(h.status());
  auto& handle = *h;
  int lwork;
  switch (type) {
    case SolverType::F32:
      JAX_THROW_IF_ERROR(
          JAX_AS_STATUS(gpusolverDnSorgqr_bufferSize(handle.get(), m, n, k,
                                                     /*A=*/nullptr,
                                                     /*lda=*/m,
                                                     /*tau=*/nullptr, &lwork)));
      break;
    case SolverType::F64:
      JAX_THROW_IF_ERROR(
          JAX_AS_STATUS(gpusolverDnDorgqr_bufferSize(handle.get(), m, n, k,
                                                     /*A=*/nullptr,
                                                     /*lda=*/m,
                                                     /*tau=*/nullptr, &lwork)));
      break;
    case SolverType::C64:
      JAX_THROW_IF_ERROR(
          JAX_AS_STATUS(gpusolverDnCungqr_bufferSize(handle.get(), m, n, k,
                                                     /*A=*/nullptr,
                                                     /*lda=*/m,
                                                     /*tau=*/nullptr, &lwork)));
      break;
    case SolverType::C128:
      JAX_THROW_IF_ERROR(
          JAX_AS_STATUS(gpusolverDnZungqr_bufferSize(handle.get(), m, n, k,
                                                     /*A=*/nullptr,
                                                     /*lda=*/m,
                                                     /*tau=*/nullptr, &lwork)));
      break;
  }
  return {lwork, PackDescriptor(OrgqrDescriptor{type, b, m, n, k, lwork})};
}

// Symmetric (Hermitian) eigendecomposition, QR algorithm: syevd/heevd

// Returns the workspace size and a descriptor for a syevd operation.
std::pair<int, py::bytes> BuildSyevdDescriptor(const py::dtype& dtype,
                                               bool lower, int b, int n) {
  SolverType type = DtypeToSolverType(dtype);
  auto h = SolverHandlePool::Borrow();
  JAX_THROW_IF_ERROR(h.status());
  auto& handle = *h;
  int lwork;
  gpusolverEigMode_t jobz = GPUSOLVER_EIG_MODE_VECTOR;
  gpusolverFillMode_t uplo =
      lower ? GPUSOLVER_FILL_MODE_LOWER : GPUSOLVER_FILL_MODE_UPPER;
  switch (type) {
    case SolverType::F32:
      JAX_THROW_IF_ERROR(JAX_AS_STATUS(gpusolverDnSsyevd_bufferSize(
          handle.get(), jobz, uplo, n, /*A=*/nullptr, /*lda=*/n, /*W=*/nullptr,
          &lwork)));
      break;
    case SolverType::F64:
      JAX_THROW_IF_ERROR(JAX_AS_STATUS(gpusolverDnDsyevd_bufferSize(
          handle.get(), jobz, uplo, n, /*A=*/nullptr, /*lda=*/n, /*W=*/nullptr,
          &lwork)));
      break;
    case SolverType::C64:
      JAX_THROW_IF_ERROR(JAX_AS_STATUS(gpusolverDnCheevd_bufferSize(
          handle.get(), jobz, uplo, n, /*A=*/nullptr, /*lda=*/n, /*W=*/nullptr,
          &lwork)));
      break;
    case SolverType::C128:
      JAX_THROW_IF_ERROR(JAX_AS_STATUS(gpusolverDnZheevd_bufferSize(
          handle.get(), jobz, uplo, n, /*A=*/nullptr, /*lda=*/n, /*W=*/nullptr,
          &lwork)));
      break;
  }
  return {lwork, PackDescriptor(SyevdDescriptor{type, uplo, b, n, lwork})};
}

// Symmetric (Hermitian) eigendecomposition, Jacobi algorithm: syevj/heevj
// Supports batches of matrices up to size 32.

// Returns the workspace size and a descriptor for a syevj_batched operation.
std::pair<int, py::bytes> BuildSyevjDescriptor(const py::dtype& dtype,
                                               bool lower, int batch, int n) {
  SolverType type = DtypeToSolverType(dtype);
  auto h = SolverHandlePool::Borrow();
  JAX_THROW_IF_ERROR(h.status());
  auto& handle = *h;
  int lwork;
  gpuSyevjInfo_t params;
  JAX_THROW_IF_ERROR(JAX_AS_STATUS(gpusolverDnCreateSyevjInfo(&params)));
  std::unique_ptr<gpuSyevjInfo, void (*)(gpuSyevjInfo_t)> params_cleanup(
      params, [](gpuSyevjInfo_t p) { gpusolverDnDestroySyevjInfo(p); });
  gpusolverEigMode_t jobz = GPUSOLVER_EIG_MODE_VECTOR;
  gpusolverFillMode_t uplo =
      lower ? GPUSOLVER_FILL_MODE_LOWER : GPUSOLVER_FILL_MODE_UPPER;
  if (batch == 1) {
    switch (type) {
      case SolverType::F32:
        JAX_THROW_IF_ERROR(JAX_AS_STATUS(gpusolverDnSsyevj_bufferSize(
            handle.get(), jobz, uplo, n, /*A=*/nullptr, /*lda=*/n,
            /*W=*/nullptr, &lwork, params)));
        break;
      case SolverType::F64:
        JAX_THROW_IF_ERROR(JAX_AS_STATUS(gpusolverDnDsyevj_bufferSize(
            handle.get(), jobz, uplo, n, /*A=*/nullptr, /*lda=*/n,
            /*W=*/nullptr, &lwork, params)));
        break;
      case SolverType::C64:
        JAX_THROW_IF_ERROR(JAX_AS_STATUS(gpusolverDnCheevj_bufferSize(
            handle.get(), jobz, uplo, n, /*A=*/nullptr, /*lda=*/n,
            /*W=*/nullptr, &lwork, params)));
        break;
      case SolverType::C128:
        JAX_THROW_IF_ERROR(JAX_AS_STATUS(gpusolverDnZheevj_bufferSize(
            handle.get(), jobz, uplo, n, /*A=*/nullptr, /*lda=*/n,
            /*W=*/nullptr, &lwork, params)));
        break;
    }
  } else {
    switch (type) {
      case SolverType::F32:
        JAX_THROW_IF_ERROR(JAX_AS_STATUS(gpusolverDnSsyevjBatched_bufferSize(
            handle.get(), jobz, uplo, n, /*A=*/nullptr, /*lda=*/n,
            /*W=*/nullptr, &lwork, params, batch)));
        break;
      case SolverType::F64:
        JAX_THROW_IF_ERROR(JAX_AS_STATUS(gpusolverDnDsyevjBatched_bufferSize(
            handle.get(), jobz, uplo, n, /*A=*/nullptr, /*lda=*/n,
            /*W=*/nullptr, &lwork, params, batch)));
        break;
      case SolverType::C64:
        JAX_THROW_IF_ERROR(JAX_AS_STATUS(gpusolverDnCheevjBatched_bufferSize(
            handle.get(), jobz, uplo, n, /*A=*/nullptr, /*lda=*/n,
            /*W=*/nullptr, &lwork, params, batch)));
        break;
      case SolverType::C128:
        JAX_THROW_IF_ERROR(JAX_AS_STATUS(gpusolverDnZheevjBatched_bufferSize(
            handle.get(), jobz, uplo, n, /*A=*/nullptr, /*lda=*/n,
            /*W=*/nullptr, &lwork, params, batch)));
        break;
    }
  }
  return {lwork, PackDescriptor(SyevjDescriptor{type, uplo, batch, n, lwork})};
}

// Singular value decomposition using QR algorithm: gesvd

// Returns the workspace size and a descriptor for a gesvd operation.
std::pair<int, py::bytes> BuildGesvdDescriptor(const py::dtype& dtype, int b,
                                               int m, int n, bool compute_uv,
                                               bool full_matrices) {
  SolverType type = DtypeToSolverType(dtype);
  auto h = SolverHandlePool::Borrow();
  JAX_THROW_IF_ERROR(h.status());
  auto& handle = *h;
  int lwork;
  signed char jobu, jobvt;
  if (compute_uv) {
    if (full_matrices) {
      jobu = jobvt = 'A';
    } else {
      jobu = jobvt = 'S';
    }
  } else {
    jobu = jobvt = 'N';
  }
  switch (type) {
    case SolverType::F32:
      JAX_THROW_IF_ERROR(JAX_AS_STATUS(gpusolverDnSgesvd_bufferSize(
          handle.get(), jobu, jobvt, m, n, &lwork)));
      break;
    case SolverType::F64:
      JAX_THROW_IF_ERROR(JAX_AS_STATUS(gpusolverDnDgesvd_bufferSize(
          handle.get(), jobu, jobvt, m, n, &lwork)));
      break;
    case SolverType::C64:
      JAX_THROW_IF_ERROR(JAX_AS_STATUS(gpusolverDnCgesvd_bufferSize(
          handle.get(), jobu, jobvt, m, n, &lwork)));
      break;
    case SolverType::C128:
      JAX_THROW_IF_ERROR(JAX_AS_STATUS(gpusolverDnZgesvd_bufferSize(
          handle.get(), jobu, jobvt, m, n, &lwork)));
      break;
  }
  return {lwork,
          PackDescriptor(GesvdDescriptor{type, b, m, n, lwork, jobu, jobvt})};
}

#ifdef JAX_GPU_CUDA

// Singular value decomposition using Jacobi algorithm: gesvdj

// Returns the workspace size and a descriptor for a gesvdj operation.
std::pair<int, py::bytes> BuildGesvdjDescriptor(const py::dtype& dtype,
                                                int batch, int m, int n,
                                                bool compute_uv, int econ) {
  SolverType type = DtypeToSolverType(dtype);
  auto h = SolverHandlePool::Borrow();
  JAX_THROW_IF_ERROR(h.status());
  auto& handle = *h;
  int lwork;
  gpusolverEigMode_t jobz =
      compute_uv ? GPUSOLVER_EIG_MODE_VECTOR : CUSOLVER_EIG_MODE_NOVECTOR;
  gesvdjInfo_t params;
  JAX_THROW_IF_ERROR(JAX_AS_STATUS(cusolverDnCreateGesvdjInfo(&params)));
  std::unique_ptr<gesvdjInfo, void (*)(gesvdjInfo*)> params_cleanup(
      params, [](gesvdjInfo* p) { cusolverDnDestroyGesvdjInfo(p); });
  if (batch == 1) {
    switch (type) {
      case SolverType::F32:
        JAX_THROW_IF_ERROR(JAX_AS_STATUS(cusolverDnSgesvdj_bufferSize(
            handle.get(), jobz, econ, m, n,
            /*A=*/nullptr, /*lda=*/m, /*S=*/nullptr,
            /*U=*/nullptr, /*ldu=*/m, /*V=*/nullptr,
            /*ldv=*/n, &lwork, params)));
        break;
      case SolverType::F64:
        JAX_THROW_IF_ERROR(JAX_AS_STATUS(cusolverDnDgesvdj_bufferSize(
            handle.get(), jobz, econ, m, n,
            /*A=*/nullptr, /*lda=*/m, /*S=*/nullptr,
            /*U=*/nullptr, /*ldu=*/m, /*V=*/nullptr,
            /*ldv=*/n, &lwork, params)));
        break;
      case SolverType::C64:
        JAX_THROW_IF_ERROR(JAX_AS_STATUS(cusolverDnCgesvdj_bufferSize(
            handle.get(), jobz, econ, m, n,
            /*A=*/nullptr, /*lda=*/m, /*S=*/nullptr,
            /*U=*/nullptr, /*ldu=*/m, /*V=*/nullptr,
            /*ldv=*/n, &lwork, params)));
        break;
      case SolverType::C128:
        JAX_THROW_IF_ERROR(JAX_AS_STATUS(cusolverDnZgesvdj_bufferSize(
            handle.get(), jobz, econ, m, n,
            /*A=*/nullptr, /*lda=*/m, /*S=*/nullptr,
            /*U=*/nullptr, /*ldu=*/m, /*V=*/nullptr,
            /*ldv=*/n, &lwork, params)));
        break;
    }
  } else {
    switch (type) {
      case SolverType::F32:
        JAX_THROW_IF_ERROR(JAX_AS_STATUS(cusolverDnSgesvdjBatched_bufferSize(
            handle.get(), jobz, m, n,
            /*A=*/nullptr, /*lda=*/m, /*S=*/nullptr,
            /*U=*/nullptr, /*ldu=*/m, /*V=*/nullptr,
            /*ldv=*/n, &lwork, params, batch)));
        break;
      case SolverType::F64:
        JAX_THROW_IF_ERROR(JAX_AS_STATUS(cusolverDnDgesvdjBatched_bufferSize(
            handle.get(), jobz, m, n,
            /*A=*/nullptr, /*lda=*/m, /*S=*/nullptr,
            /*U=*/nullptr, /*ldu=*/m, /*V=*/nullptr,
            /*ldv=*/n, &lwork, params, batch)));
        break;
      case SolverType::C64:
        JAX_THROW_IF_ERROR(JAX_AS_STATUS(cusolverDnCgesvdjBatched_bufferSize(
            handle.get(), jobz, m, n,
            /*A=*/nullptr, /*lda=*/m, /*S=*/nullptr,
            /*U=*/nullptr, /*ldu=*/m, /*V=*/nullptr,
            /*ldv=*/n, &lwork, params, batch)));
        break;
      case SolverType::C128:
        JAX_THROW_IF_ERROR(JAX_AS_STATUS(cusolverDnZgesvdjBatched_bufferSize(
            handle.get(), jobz, m, n,
            /*A=*/nullptr, /*lda=*/m, /*S=*/nullptr,
            /*U=*/nullptr, /*ldu=*/m, /*V=*/nullptr,
            /*ldv=*/n, &lwork, params, batch)));
        break;
    }
  }
  return {lwork, PackDescriptor(
                     GesvdjDescriptor{type, batch, m, n, lwork, jobz, econ})};
}

#endif  // JAX_GPU_CUDA

// Returns the workspace size and a descriptor for a geqrf operation.
std::pair<int, py::bytes> BuildSytrdDescriptor(const py::dtype& dtype,
                                               bool lower, int b, int n) {
  SolverType type = DtypeToSolverType(dtype);
  auto h = SolverHandlePool::Borrow();
  JAX_THROW_IF_ERROR(h.status());
  auto& handle = *h;
  int lwork;
  gpusolverFillMode_t uplo =
      lower ? GPUSOLVER_FILL_MODE_LOWER : GPUSOLVER_FILL_MODE_UPPER;
  switch (type) {
    case SolverType::F32:
      JAX_THROW_IF_ERROR(JAX_AS_STATUS(gpusolverDnSsytrd_bufferSize(
          handle.get(), uplo, n, /*A=*/nullptr, /*lda=*/n, /*D=*/nullptr,
          /*E=*/nullptr, /*tau=*/nullptr, &lwork)));
      break;
    case SolverType::F64:
      JAX_THROW_IF_ERROR(JAX_AS_STATUS(gpusolverDnDsytrd_bufferSize(
          handle.get(), uplo, n, /*A=*/nullptr, /*lda=*/n, /*D=*/nullptr,
          /*E=*/nullptr, /*tau=*/nullptr, &lwork)));
      break;
    case SolverType::C64:
      JAX_THROW_IF_ERROR(JAX_AS_STATUS(gpusolverDnChetrd_bufferSize(
          handle.get(), uplo, n, /*A=*/nullptr, /*lda=*/n, /*D=*/nullptr,
          /*E=*/nullptr, /*tau=*/nullptr, &lwork)));
      break;
    case SolverType::C128:
      JAX_THROW_IF_ERROR(JAX_AS_STATUS(gpusolverDnZhetrd_bufferSize(
          handle.get(), uplo, n, /*A=*/nullptr, /*lda=*/n, /*D=*/nullptr,
          /*E=*/nullptr, /*tau=*/nullptr, &lwork)));
      break;
  }
  return {lwork, PackDescriptor(SytrdDescriptor{type, uplo, b, n, n, lwork})};
}

py::dict Registrations() {
  py::dict dict;
  dict[JAX_GPU_PREFIX "solver_potrf"] = EncapsulateFunction(Potrf);
  dict[JAX_GPU_PREFIX "solver_getrf"] = EncapsulateFunction(Getrf);
  dict[JAX_GPU_PREFIX "solver_geqrf"] = EncapsulateFunction(Geqrf);
  dict[JAX_GPU_PREFIX "solver_orgqr"] = EncapsulateFunction(Orgqr);
  dict[JAX_GPU_PREFIX "solver_syevd"] = EncapsulateFunction(Syevd);
  dict[JAX_GPU_PREFIX "solver_syevj"] = EncapsulateFunction(Syevj);
  dict[JAX_GPU_PREFIX "solver_gesvd"] = EncapsulateFunction(Gesvd);
  dict[JAX_GPU_PREFIX "solver_sytrd"] = EncapsulateFunction(Sytrd);

#ifdef JAX_GPU_CUDA
  dict["cusolver_csrlsvqr"] = EncapsulateFunction(Csrlsvqr);
  dict["cusolver_gesvdj"] = EncapsulateFunction(Gesvdj);
#endif  // JAX_GPU_CUDA
  return dict;
}

PYBIND11_MODULE(_solver, m) {
  m.def("registrations", &Registrations);
  m.def("build_potrf_descriptor", &BuildPotrfDescriptor);
  m.def("build_getrf_descriptor", &BuildGetrfDescriptor);
  m.def("build_geqrf_descriptor", &BuildGeqrfDescriptor);
  m.def("build_orgqr_descriptor", &BuildOrgqrDescriptor);
  m.def("build_syevd_descriptor", &BuildSyevdDescriptor);
  m.def("build_syevj_descriptor", &BuildSyevjDescriptor);
  m.def("build_gesvd_descriptor", &BuildGesvdDescriptor);
  m.def("build_sytrd_descriptor", &BuildSytrdDescriptor);
#ifdef JAX_GPU_CUDA
  m.def("build_csrlsvqr_descriptor", &BuildCsrlsvqrDescriptor);
  m.def("build_gesvdj_descriptor", &BuildGesvdjDescriptor);
#endif  // JAX_GPU_CUDA
}

}  // namespace
}  // namespace JAX_GPU_NAMESPACE
}  // namespace jax
