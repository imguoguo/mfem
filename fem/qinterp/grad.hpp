// Copyright (c) 2010-2023, Lawrence Livermore National Security, LLC. Produced
// at the Lawrence Livermore National Laboratory. All Rights reserved. See files
// LICENSE and NOTICE for details. LLNL-CODE-806117.
//
// This file is part of the MFEM library. For more information and source code
// availability visit https://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the BSD-3 license. We welcome feedback and contributions, see file
// CONTRIBUTING.md for details.

// Internal header, included only by .cpp files.
// Template function implementations.

#include "../quadinterpolator.hpp"
#include "../../general/forall.hpp"
#include "../../linalg/dtensor.hpp"
#include "../../fem/kernels.hpp"
#include "../../linalg/kernels.hpp"

namespace mfem
{

namespace internal
{

namespace quadrature_interpolator
{

template<QVectorLayout Q_LAYOUT, bool GRAD_PHYS>
static void Derivatives1D(const int NE,
                          const double *g_,
                          const double *j_,
                          const double *x_,
                          double *y_,
                          const int sdim,
                          const int vdim,
                          const int d1d,
                          const int q1d)
{
   const auto g = Reshape(g_, q1d, d1d);
   const auto j = Reshape(j_, q1d, sdim, NE);
   const auto x = Reshape(x_, d1d, vdim, NE);
   auto y = Q_LAYOUT == QVectorLayout::byNODES ?
            Reshape(y_, q1d, vdim, sdim, NE):
            Reshape(y_, vdim, sdim, q1d, NE);

   mfem::forall(NE, [=] MFEM_HOST_DEVICE (int e)
   {
      for (int c = 0; c < vdim; c++)
      {
         for (int q = 0; q < q1d; q++)
         {
            double du[3] = {0.0, 0.0, 0.0};
            for (int d = 0; d < d1d; d++)
            {
               du[0] += g(q, d) * x(d, c, e);
            }
            if (GRAD_PHYS)
            {
               if (sdim == 1) { du[0] /= j(q, 0, e); }
               else if (sdim == 2)
               {
                  const double Jloc[2] = {j(q,0,e), j(q,1,e)};
                  double Jinv[3];
                  kernels::CalcLeftInverse<2,1>(Jloc, Jinv);
                  const double U = Jinv[0]*du[0];
                  const double V = Jinv[1]*du[0];
                  du[0] = U;
                  du[1] = V;
               }
               else // sdim == 3
               {
                  const double Jloc[3] = {j(q,0,e), j(q,1,e), j(q,2,e)};
                  double Jinv[3];
                  kernels::CalcLeftInverse<3,1>(Jloc, Jinv);
                  const double U = Jinv[0]*du[0];
                  const double V = Jinv[1]*du[0];
                  const double W = Jinv[2]*du[0];
                  du[0] = U;
                  du[1] = V;
                  du[2] = W;
               }
            }
            for (int d = 0; d < sdim; ++d)
            {
               if (Q_LAYOUT == QVectorLayout::byVDIM)  { y(c, d, q, e) = du[d]; }
               if (Q_LAYOUT == QVectorLayout::byNODES) { y(q, c, d, e) = du[d]; }
            }
         }
      }
   });
}

// Template compute kernel for derivatives in 2D: tensor product version.
template<QVectorLayout Q_LAYOUT, bool GRAD_PHYS,
         int T_VDIM = 0, int T_D1D = 0, int T_Q1D = 0,
         int T_NBZ = 1, int MAX_D1D = 0, int MAX_Q1D = 0>
static void Derivatives2D(const int NE,
                          const double *b_,
                          const double *g_,
                          const double *j_,
                          const double *x_,
                          double *y_,
                          const int sdim = 2,
                          const int vdim = 0,
                          const int d1d = 0,
                          const int q1d = 0)
{
   const int D1D = T_D1D ? T_D1D : d1d;
   const int Q1D = T_Q1D ? T_Q1D : q1d;
   const int VDIM = T_VDIM ? T_VDIM : vdim;
   const int SDIM = GRAD_PHYS ? sdim : 2;
   static constexpr int NBZ = T_NBZ ? T_NBZ : 1;

   const auto b = Reshape(b_, Q1D, D1D);
   const auto g = Reshape(g_, Q1D, D1D);
   const auto j = Reshape(j_, Q1D, Q1D, SDIM, 2, NE);
   const auto x = Reshape(x_, D1D, D1D, VDIM, NE);
   auto y = Q_LAYOUT == QVectorLayout:: byNODES ?
            Reshape(y_, Q1D, Q1D, VDIM, SDIM, NE):
            Reshape(y_, VDIM, SDIM, Q1D, Q1D, NE);

   mfem::forall_2D_batch(NE, Q1D, Q1D, NBZ, [=] MFEM_HOST_DEVICE (int e)
   {
      const int D1D = T_D1D ? T_D1D : d1d;
      const int Q1D = T_Q1D ? T_Q1D : q1d;
      const int VDIM = T_VDIM ? T_VDIM : vdim;
      constexpr int MQ1 = T_Q1D ? T_Q1D : MAX_Q1D;
      constexpr int MD1 = T_D1D ? T_D1D : MAX_D1D;

      const int tidz = MFEM_THREAD_ID(z);
      MFEM_SHARED double BG[2][MQ1*MD1];
      kernels::internal::LoadBG<MD1,MQ1>(D1D,Q1D,b,g,BG);
      DeviceMatrix B(BG[0], D1D, Q1D);
      DeviceMatrix G(BG[1], D1D, Q1D);

      MFEM_SHARED double XY[NBZ][MD1*MD1];
      DeviceTensor<2> X((double*)(XY+tidz), D1D, D1D);

      MFEM_SHARED double s_DQ[2][NBZ][MD1*MQ1];
      DeviceTensor<2> DQ0(s_DQ[0][tidz], D1D, Q1D);
      DeviceTensor<2> DQ1(s_DQ[1][tidz], D1D, Q1D);

      for (int c = 0; c < VDIM; ++c)
      {
         kernels::internal::LoadX<MD1,NBZ>(e,D1D,c,x,XY);
         MFEM_FOREACH_THREAD(dy,y,D1D)
         {
            MFEM_FOREACH_THREAD(qx,x,Q1D)
            {
               double u = 0.0;
               double v = 0.0;
               for (int dx = 0; dx < D1D; ++dx)
               {
                  const double input = X(dx,dy);
                  u += input * B(dx,qx);
                  v += input * G(dx,qx);
               }
               DQ0(dy,qx) = u;
               DQ1(dy,qx) = v;
            }
         }
         MFEM_SYNC_THREAD;
         MFEM_FOREACH_THREAD(qy,y,Q1D)
         {
            MFEM_FOREACH_THREAD(qx,x,Q1D)
            {
               double du[3] = {0.0, 0.0, 0.0};
               for (int dy = 0; dy < D1D; ++dy)
               {
                  du[0] += DQ1(dy,qx) * B(dy,qy);
                  du[1] += DQ0(dy,qx) * G(dy,qy);
               }
               if (GRAD_PHYS)
               {
                  if (SDIM == 2)
                  {
                     double Jloc[4], Jinv[4];
                     Jloc[0] = j(qx,qy,0,0,e);
                     Jloc[1] = j(qx,qy,1,0,e);
                     Jloc[2] = j(qx,qy,0,1,e);
                     Jloc[3] = j(qx,qy,1,1,e);
                     kernels::CalcInverse<2>(Jloc, Jinv);
                     const double U = Jinv[0]*du[0] + Jinv[1]*du[1];
                     const double V = Jinv[2]*du[0] + Jinv[3]*du[1];
                     du[0] = U;
                     du[1] = V;
                  }
                  else
                  {
                     double Jloc[6], Jinv[6];
                     Jloc[0] = j(qx,qy,0,0,e);
                     Jloc[1] = j(qx,qy,1,0,e);
                     Jloc[2] = j(qx,qy,2,0,e);
                     Jloc[3] = j(qx,qy,0,1,e);
                     Jloc[4] = j(qx,qy,1,1,e);
                     Jloc[5] = j(qx,qy,2,1,e);
                     kernels::CalcLeftInverse<3,2>(Jloc, Jinv);
                     const double U = Jinv[0]*du[0] + Jinv[1]*du[1];
                     const double V = Jinv[2]*du[0] + Jinv[3]*du[1];
                     const double W = Jinv[4]*du[0] + Jinv[5]*du[1];
                     du[0] = U;
                     du[1] = V;
                     du[2] = W;
                  }
               }
               for (int d = 0; d < SDIM; ++d)
               {
                  if (Q_LAYOUT == QVectorLayout::byVDIM)
                  {
                     y(c,d,qx,qy,e) = du[d];
                  }
                  else // Q_LAYOUT == QVectorLayout::byNODES
                  {
                     y(qx,qy,c,d,e) = du[d];
                  }
               }
            }
         }
         MFEM_SYNC_THREAD;
      }
   });
}

// Template compute kernel for derivatives in 3D: tensor product version.
template<QVectorLayout Q_LAYOUT, bool GRAD_PHYS,
         int T_VDIM = 0, int T_D1D = 0, int T_Q1D = 0,
         int MAX_D1D = 0, int MAX_Q1D = 0>
static void Derivatives3D(const int NE,
                          const double *b_,
                          const double *g_,
                          const double *j_,
                          const double *x_,
                          double *y_,
                          const int vdim = 0,
                          const int d1d = 0,
                          const int q1d = 0)
{
   const int D1D = T_D1D ? T_D1D : d1d;
   const int Q1D = T_Q1D ? T_Q1D : q1d;
   const int VDIM = T_VDIM ? T_VDIM : vdim;

   const auto b = Reshape(b_, Q1D, D1D);
   const auto g = Reshape(g_, Q1D, D1D);
   const auto j = Reshape(j_, Q1D, Q1D, Q1D, 3, 3, NE);
   const auto x = Reshape(x_, D1D, D1D, D1D, VDIM, NE);
   auto y = Q_LAYOUT == QVectorLayout:: byNODES ?
            Reshape(y_, Q1D, Q1D, Q1D, VDIM, 3, NE):
            Reshape(y_, VDIM, 3, Q1D, Q1D, Q1D, NE);

   mfem::forall_3D(NE, Q1D, Q1D, Q1D, [=] MFEM_HOST_DEVICE (int e)
   {
      const int D1D = T_D1D ? T_D1D : d1d;
      const int Q1D = T_Q1D ? T_Q1D : q1d;
      const int VDIM = T_VDIM ? T_VDIM : vdim;
      constexpr int MQ1 = T_Q1D ? T_Q1D : MAX_Q1D;
      constexpr int MD1 = T_D1D ? T_D1D : MAX_D1D;

      MFEM_SHARED double BG[2][MQ1*MD1];
      kernels::internal::LoadBG<MD1,MQ1>(D1D,Q1D,b,g,BG);
      DeviceMatrix B(BG[0], D1D, Q1D);
      DeviceMatrix G(BG[1], D1D, Q1D);

      MFEM_SHARED double sm0[3][MQ1*MQ1*MQ1];
      MFEM_SHARED double sm1[3][MQ1*MQ1*MQ1];
      DeviceTensor<3> X(sm0[2], D1D, D1D, D1D);
      DeviceTensor<3> DDQ0(sm0[0], D1D, D1D, Q1D);
      DeviceTensor<3> DDQ1(sm0[1], D1D, D1D, Q1D);
      DeviceTensor<3> DQQ0(sm1[0], D1D, Q1D, Q1D);
      DeviceTensor<3> DQQ1(sm1[1], D1D, Q1D, Q1D);
      DeviceTensor<3> DQQ2(sm1[2], D1D, Q1D, Q1D);

      for (int c = 0; c < VDIM; ++c)
      {
         kernels::internal::LoadX(e,D1D,c,x,X);
         MFEM_FOREACH_THREAD(dz,z,D1D)
         {
            MFEM_FOREACH_THREAD(dy,y,D1D)
            {
               MFEM_FOREACH_THREAD(qx,x,Q1D)
               {
                  double u = 0.0;
                  double v = 0.0;
                  for (int dx = 0; dx < D1D; ++dx)
                  {
                     const double input = X(dx,dy,dz);
                     u += input * B(dx,qx);
                     v += input * G(dx,qx);
                  }
                  DDQ0(dz,dy,qx) = u;
                  DDQ1(dz,dy,qx) = v;
               }
            }
         }
         MFEM_SYNC_THREAD;
         MFEM_FOREACH_THREAD(dz,z,D1D)
         {
            MFEM_FOREACH_THREAD(qy,y,Q1D)
            {
               MFEM_FOREACH_THREAD(qx,x,Q1D)
               {
                  double u = 0.0;
                  double v = 0.0;
                  double w = 0.0;
                  for (int dy = 0; dy < D1D; ++dy)
                  {
                     u += DDQ1(dz,dy,qx) * B(dy,qy);
                     v += DDQ0(dz,dy,qx) * G(dy,qy);
                     w += DDQ0(dz,dy,qx) * B(dy,qy);
                  }
                  DQQ0(dz,qy,qx) = u;
                  DQQ1(dz,qy,qx) = v;
                  DQQ2(dz,qy,qx) = w;
               }
            }
         }
         MFEM_SYNC_THREAD;
         MFEM_FOREACH_THREAD(qz,z,Q1D)
         {
            MFEM_FOREACH_THREAD(qy,y,Q1D)
            {
               MFEM_FOREACH_THREAD(qx,x,Q1D)
               {
                  double u = 0.0;
                  double v = 0.0;
                  double w = 0.0;
                  for (int dz = 0; dz < D1D; ++dz)
                  {
                     u += DQQ0(dz,qy,qx) * B(dz,qz);
                     v += DQQ1(dz,qy,qx) * B(dz,qz);
                     w += DQQ2(dz,qy,qx) * G(dz,qz);
                  }
                  if (GRAD_PHYS)
                  {
                     double Jloc[9], Jinv[9];
                     for (int col = 0; col < 3; col++)
                     {
                        for (int row = 0; row < 3; row++)
                        {
                           Jloc[row+3*col] = j(qx,qy,qz,row,col,e);
                        }
                     }
                     kernels::CalcInverse<3>(Jloc, Jinv);
                     const double U = Jinv[0]*u + Jinv[1]*v + Jinv[2]*w;
                     const double V = Jinv[3]*u + Jinv[4]*v + Jinv[5]*w;
                     const double W = Jinv[6]*u + Jinv[7]*v + Jinv[8]*w;
                     u = U; v = V; w = W;
                  }
                  if (Q_LAYOUT == QVectorLayout::byVDIM)
                  {
                     y(c,0,qx,qy,qz,e) = u;
                     y(c,1,qx,qy,qz,e) = v;
                     y(c,2,qx,qy,qz,e) = w;
                  }
                  if (Q_LAYOUT == QVectorLayout::byNODES)
                  {
                     y(qx,qy,qz,c,0,e) = u;
                     y(qx,qy,qz,c,1,e) = v;
                     y(qx,qy,qz,c,2,e) = w;
                  }
               }
            }
         }
         MFEM_SYNC_THREAD;
      }
   });
}

} // namespace quadrature_interpolator

} // namespace internal

} // namespace mfem
