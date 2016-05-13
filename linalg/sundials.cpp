#include "sundials.hpp"

#ifdef MFEM_USE_SUNDIALS

#include "solvers.hpp"

#include <cvode/cvode_band.h>
#include <cvode/cvode_spgmr.h>
#include <nvector/nvector_serial.h>
#ifdef MFEM_USE_MPI
#include "hypre.hpp"
#include <nvector/nvector_parhyp.h>
#endif
#include <arkode/arkode_impl.h>
#include <cvode/cvode_impl.h>


/* Choose default tolerances to match ARKode defaults*/
#define RELTOL RCONST(1.0e-4)
#define ABSTOL RCONST(1.0e-9)

using namespace std;

static int SundialsMult(realtype t, N_Vector y, N_Vector ydot, void *user_data)
{
   mfem::TimeDependentOperator *f =
      static_cast<mfem::TimeDependentOperator *>(user_data);

   // Creates mfem Vectors linked to the data in y and in ydot.
#ifndef MFEM_USE_MPI
   mfem::Vector mfem_y(NV_DATA_S(y), NV_LENGTH_S(y));
   mfem::Vector mfem_ydot(NV_DATA_S(ydot), NV_LENGTH_S(ydot));
#else
   mfem::HypreParVector mfem_y =
      static_cast<mfem::HypreParVector>(NV_HYPRE_PARVEC_PH(y));
   mfem::HypreParVector mfem_ydot =
      static_cast<mfem::HypreParVector>(NV_HYPRE_PARVEC_PH(ydot));
#endif

   // Apply y' = f(t, y).
   f->SetTime(t);
   f->Mult(mfem_y, mfem_ydot);

   return 0;
}

/// Linear solve associated with CVodeMem structs.
static int MFEMLinearCVSolve(void *cvode_mem, mfem::Solver* solve,
                             mfem::SundialsLinearSolveOperator* op);

/// Linear solve associated with ARKodeMem structs.
static int MFEMLinearARKSolve(void *arkode_mem, mfem::Solver*,
                              mfem::SundialsLinearSolveOperator*);

namespace mfem
{

CVODESolver::CVODESolver(Vector &_y, int lmm, int iter)
   : ODESolver(),
     tolerances_set_sundials(false),
     solver_iteration_type(iter)
{
   // Create the NVector y.
   CreateNVector(&_y);

   // Create the solver memory.
   ode_mem = CVodeCreate(lmm, iter);
}

#ifdef MFEM_USE_MPI
CVODESolver::CVODESolver(MPI_Comm _comm, Vector &_y, int lmm, int iter)
   : ODESolver(),
     comm(_comm),
     tolerances_set_sundials(false),
     solver_iteration_type(iter)
{
   // Create the NVector y.
   CreateNVector(&_y);

   // Create the solver memory.
   ode_mem = CVodeCreate(lmm, iter);
}
#endif

void CVODESolver::Init(TimeDependentOperator &_f)
{
   f = &_f;

   // Initialize integrator memory, specify the user's
   // RHS function in x' = f(t, x), initial time, initial condition.
   int flag = CVodeInit(ode_mem, SundialsMult, 0.0, y);

   MFEM_ASSERT(flag >= 0, "CVodeInit() failed!");

   SetSStolerances(RELTOL, ABSTOL);

   // Set the pointer to user-defined data.
   flag = CVodeSetUserData(ode_mem, f);
   MFEM_ASSERT(flag >= 0, "CVodeSetUserData() failed!");

   // TODO: what's going on with the tolerances. Should this be in Init()?
   if (solver_iteration_type == CV_NEWTON)
   {
#ifndef MFEM_USE_MPI
      double size = f->Width();
      SetSStolerances(1e-3, 1e-6);
      CVBand(ode_mem, size, size * 0.5, size * 0.5);
#else
      SetSStolerances(1e-3, 1e-6);
      CVSpgmr(ode_mem, PREC_NONE, 0);
#endif
   }
}

void CVODESolver::ReInit(TimeDependentOperator &_f, Vector &_y, double & _t)
{
   f = &_f;
   CreateNVector(&_y);

   // Re-init memory, time and solution. The RHS action is known from Init().
   int flag = CVodeReInit(ode_mem, static_cast<realtype>(_t), y);
   MFEM_ASSERT(flag >= 0, "CVodeReInit() failed!");

   // Set the pointer to user-defined data.
   flag = CVodeSetUserData(ode_mem, this->f);
   MFEM_ASSERT(flag >= 0, "CVodeSetUserData() failed!");

   // TODO: what's going on with the tolerances. Should this be in Init()?
   if (solver_iteration_type == CV_NEWTON)
   {
#ifndef MFEM_USE_MPI
      double size = f->Width();
      SetSStolerances(1e-3, 1e-6);
      CVBand(ode_mem, size, size * 0.5, size * 0.5);
#else
      SetSStolerances(1e-3, 1e-6);
      CVSpgmr(ode_mem, PREC_NONE, 0);
#endif
   }
}

void CVODESolver::SetSStolerances(realtype reltol, realtype abstol)
{
   // Specify the scalar relative tolerance and scalar absolute tolerance.
   int flag = CVodeSStolerances(ode_mem, reltol, abstol);
   MFEM_ASSERT(flag >= 0, "CVodeSStolerances() failed!");

   tolerances_set_sundials = true;
}

void CVODESolver::Step(Vector &x, double &t, double &dt)
{
   TransferNVectorShallow(&x, y);

   // Perform the step.
   realtype tout = t + dt;
   int flag = CVode(ode_mem, tout, y, &t, CV_NORMAL);
   MFEM_ASSERT(flag >= 0, "CVode() failed!");

   // Record last incremental step size.
   flag = CVodeGetLastStep(ode_mem, &dt);
}

void CVODESolver::CreateNVector(Vector* _y)
{
#ifndef MFEM_USE_MPI
   // Create a serial NVector.
   y = N_VMake_Serial(_y->Size(),
                      (realtype*) _y->GetData());   /* Allocate y vector */
   MFEM_ASSERT(static_cast<void *>(y) != NULL, "N_VMake_Serial() failed!");
#else
   // Create a parallel NVector.
   HypreParVector *x = dynamic_cast<HypreParVector *>(_y);
   MFEM_ASSERT(x != NULL, "Could not cast to HypreParVector!");

   y = N_VMake_ParHyp(x->StealParVector());
#endif
}

void CVODESolver::TransferNVectorShallow(Vector* _x, N_Vector &_y)
{
#ifndef MFEM_USE_MPI
   NV_DATA_S(_y) = _x->GetData();
#else
   HypreParVector *x = dynamic_cast<HypreParVector *>(_x);
   MFEM_ASSERT(x != NULL, "Could not cast to HypreParVector!");

   y = N_VMake_ParHyp(x->StealParVector());
#endif
}

void CVODESolver::SetLinearSolve(Solver* J_solve,
                                 SundialsLinearSolveOperator* op)
{
   // Jane comment: If linear solve should be Newton, recreate ode_mem object.
   //    Consider checking for CV_ADAMS vs CV_BDF as well.
   // TODO: Is there an error here ??
   if (solver_iteration_type == CV_FUNCTIONAL)
   {
      realtype t0 = ((CVodeMem) ode_mem)->cv_tn;
      CVodeFree(&ode_mem);
      ode_mem=CVodeCreate(CV_BDF, CV_NEWTON);
      tolerances_set_sundials=false;

      int flag = CVodeInit(ode_mem, SundialsMult, t0, y);
      MFEM_ASSERT(flag >= 0, "CVodeInit() failed!");

      CVodeSetUserData(ode_mem, this->f);
      if (!tolerances_set_sundials)
      {
         SetSStolerances(RELTOL,ABSTOL);
      }
   }

   // Call CVodeSetMaxNumSteps to increase default.
   CVodeSetMaxNumSteps(ode_mem, 10000);
   SetSStolerances(1e-2,1e-4);

   MFEMLinearCVSolve(ode_mem, J_solve, op);
}

CVODESolver::~CVODESolver()
{
   N_VDestroy(y);
   if (ode_mem != NULL)
   {
      CVodeFree(&ode_mem);
   }
}

ARKODESolver::ARKODESolver(Vector &_y, int _use_explicit)
   : ODESolver(),
     tolerances_set_sundials(false),
     use_explicit(_use_explicit)
{
   y = N_VMake_Serial(_y.Size(),
                      (realtype*) _y.GetData());   /* Allocate y vector */
   MFEM_ASSERT(static_cast<void *>(y) != NULL, "N_VMake_Serial() failed!");

   // Create the solver memory.
   ode_mem = ARKodeCreate();
}

#ifdef MFEM_USE_MPI
ARKODESolver::ARKODESolver(MPI_Comm _comm, Vector &_y, int _use_explicit)
   : ODESolver(),
     comm(_comm),
     tolerances_set_sundials(false),
     use_explicit(_use_explicit)
{
   // Create the NVector y.
   CreateNVector(&_y);

   // Create the solver memory.
   ode_mem = ARKodeCreate();
}
#endif

void ARKODESolver::Init(TimeDependentOperator &_f)
{
   f = &_f;

   // Initialize the integrator memory, specify the user's
   // RHS function in x' = f(t, x), the inital time, initial condition.
   int flag = use_explicit ?
              ARKodeInit(ode_mem, SundialsMult, NULL, 0.0, y) :
              ARKodeInit(ode_mem, NULL, SundialsMult, 0.0, y);
   MFEM_ASSERT(flag >= 0, "ARKodeInit() failed!");

   SetSStolerances(RELTOL, ABSTOL);

   /* Set the pointer to user-defined data */
   flag = ARKodeSetUserData(ode_mem, this->f);
   MFEM_ASSERT(flag >= 0, "ARKodeSetUserData() failed!");
}

void ARKODESolver::ReInit(TimeDependentOperator &_f, Vector &_y, double &_t)
{
   f = &_f;
   CreateNVector(&_y);

   // Re-init memory, time and solution. The RHS action is known from Init().
   int flag = use_explicit ?
              ARKodeReInit(ode_mem, SundialsMult, NULL, (realtype) _t, y) :
              ARKodeReInit(ode_mem, NULL, SundialsMult, (realtype) _t, y);
   MFEM_ASSERT(flag >= 0, "ARKodeReInit() failed!");

   // Set the pointer to user-defined data.
   flag = ARKodeSetUserData(ode_mem, this->f);
   MFEM_ASSERT(flag >= 0, "ARKodeSetUserData() failed!");
}

void ARKODESolver::SetSStolerances(realtype reltol, realtype abstol)
{
   // Specify the scalar relative tolerance and scalar absolute tolerance.
   int flag = ARKodeSStolerances(ode_mem, reltol, abstol);
   MFEM_ASSERT(flag >= 0, "ARKodeSStolerances() failed!");

   tolerances_set_sundials = true;
}

void ARKODESolver::Step(Vector &x, double &t, double &dt)
{
   TransferNVectorShallow(&x, y);

   // Step.
   realtype tout = t + dt;
   int flag = ARKode(ode_mem, tout, y, &t, ARK_NORMAL);
   MFEM_ASSERT(flag >= 0, "ARKode() failed!");

   // Record last incremental step size.
   flag = ARKodeGetLastStep(ode_mem, &dt);
}

void ARKODESolver::CreateNVector(Vector* _y)
{
#ifdef MFEM_USE_MPI
   HypreParVector *x = dynamic_cast<HypreParVector *>(_y);
   MFEM_ASSERT(x != NULL, "Could not cast to HypreParVector!");
   y = N_VMake_ParHyp(x->StealParVector());
#else
   // Create a serial vector
   y = N_VMake_Serial(_y->Size(),
                      (realtype*) _y->GetData());   /* Allocate y vector */
   MFEM_ASSERT(static_cast<void *>(y) != NULL, "N_VMake_Serial() failed!");
#endif
}

void ARKODESolver::TransferNVectorShallow(Vector* _x, N_Vector &_y)
{
#ifdef MFEM_USE_MPI
   HypreParVector *x = dynamic_cast<HypreParVector *>(_x);
   MFEM_ASSERT(x != NULL, "Could not cast to HypreParVector!");
   y = N_VMake_ParHyp(x->StealParVector());
#else
   NV_DATA_S(_y) = _x->GetData();
#endif
}

void ARKODESolver::WrapSetERKTableNum(int table_num)
{
   ARKodeSetERKTableNum(ode_mem, table_num);
}

void ARKODESolver::WrapSetFixedStep(double dt)
{
   ARKodeSetFixedStep(ode_mem, static_cast<realtype>(dt));
}

void ARKODESolver::SetLinearSolve(Solver* solve,
                                  SundialsLinearSolveOperator* op)
{
   if (use_explicit)
   {
      realtype t0= ((ARKodeMem) ode_mem)->ark_tn;
      ARKodeFree(&ode_mem);
      ode_mem=ARKodeCreate();
      tolerances_set_sundials=false;
      // TODO: why is this?
      use_explicit=false;
      //change init structure in order to switch to implicit method
      int flag = use_explicit ?
                    ARKodeInit(ode_mem, SundialsMult, NULL, t0, y) :
                    ARKodeInit(ode_mem, NULL, SundialsMult, t0, y);
      MFEM_ASSERT(flag >= 0, "ARKodeInit() failed!");

      ARKodeSetUserData(ode_mem, this->f);
      if (!tolerances_set_sundials)
      {
         SetSStolerances(RELTOL, ABSTOL);
      }
   }

   // Call ARKodeSetMaxNumSteps to increase default.
   ARKodeSetMaxNumSteps(ode_mem, 10000);
   SetSStolerances(1e-2,1e-4);
   MFEMLinearARKSolve(ode_mem, solve, op);
}

ARKODESolver::~ARKODESolver()
{
   N_VDestroy(y);
   if (ode_mem != NULL)
   {
      ARKodeFree(&ode_mem);
   }
}


} // namespace mfem

static int WrapLinearSolveSetup(void* lmem, double tn,
                                mfem::Vector* ypred, mfem::Vector* fpred)
{
   return 0;
}

static int WrapLinearSolve(void* lmem, double tn, mfem::Vector* b,
                           mfem::Vector* ycur, mfem::Vector* yn,
                           mfem::Vector* fcur)
{
   mfem::MFEMLinearSolverMemory *tmp_lmem =
         static_cast<mfem::MFEMLinearSolverMemory *>(lmem);

   tmp_lmem->op_for_gradient->SolveJacobian(b, ycur, yn, tmp_lmem->J_solve,
                                            tmp_lmem->weight);
   return 0;
}

static int WrapLinearCVSolveInit(CVodeMem cv_mem)
{
   return 0;
}

//Setup may not be needed, since Jacobian is recomputed each iteration
//ypred is the predicted y at the current time, fpred is f(t,ypred)
static int WrapLinearCVSolveSetup(CVodeMem cv_mem, int convfail,
                                  N_Vector ypred, N_Vector fpred,
                                  booleantype *jcurPtr, N_Vector vtemp1,
                                  N_Vector vtemp2, N_Vector vtemp3)
{
   mfem::MFEMLinearSolverMemory* lmem= (mfem::MFEMLinearSolverMemory*)
                                       cv_mem->cv_lmem;
#ifndef MFEM_USE_MPI
   lmem->setup_y->SetDataAndSize(NV_DATA_S(ypred),NV_LENGTH_S(ypred));
   lmem->setup_f->SetDataAndSize(NV_DATA_S(fpred),NV_LENGTH_S(fpred));
#else
   lmem->setup_y->SetData(NV_DATA_PH(ypred));
   lmem->setup_f->SetData(NV_DATA_PH(fpred));
#endif
   *jcurPtr=TRUE;
   cv_mem->cv_lmem = lmem;
   WrapLinearSolveSetup(cv_mem->cv_lmem, cv_mem->cv_tn, lmem->setup_y,
                        lmem->setup_f);
   return 0;
}

static int WrapLinearCVSolve(CVodeMem cv_mem, N_Vector b,
                             N_Vector weight, N_Vector ycur,
                             N_Vector fcur)
{
   mfem::MFEMLinearSolverMemory* lmem= (mfem::MFEMLinearSolverMemory*)
                                       cv_mem->cv_lmem;
#ifndef MFEM_USE_MPI
   lmem->solve_y->SetDataAndSize(NV_DATA_S(ycur),NV_LENGTH_S(ycur));
   lmem->solve_yn->SetDataAndSize(NV_DATA_S(cv_mem->cv_zn[0]),NV_LENGTH_S(ycur));
   lmem->solve_f->SetDataAndSize(NV_DATA_S(fcur),NV_LENGTH_S(fcur));
   lmem->solve_b->SetDataAndSize(NV_DATA_S(b),NV_LENGTH_S(b));
#else
   ((mfem::HypreParVector*) lmem->solve_y)->SetDataAndSize(NV_DATA_PH(ycur),
                                                           NV_LOCLENGTH_PH(ycur));
   ((mfem::HypreParVector*) lmem->solve_f)->SetDataAndSize(NV_DATA_PH(fcur),
                                                           NV_LOCLENGTH_PH(fcur));
   ((mfem::HypreParVector*) lmem->solve_b)->SetDataAndSize(NV_DATA_PH(b),
                                                           NV_LOCLENGTH_PH(b));
#endif

   lmem->weight = cv_mem->cv_gamma;

   cv_mem->cv_lmem = lmem;
   WrapLinearSolve(cv_mem->cv_lmem, cv_mem->cv_tn, lmem->solve_b, lmem->solve_y,
                   lmem->setup_y, lmem->solve_f);
   return 0;
}

static void WrapLinearCVSolveFree(CVodeMem cv_mem)
{
   return;
}

/*---------------------------------------------------------------
 MFEMLinearCVSolve:

 This routine initializes the memory record and sets various
 function fields specific to the linear solver module.
 MFEMLinearCVSolve first calls the existing lfree routine if this is not
 NULL. It then sets the cv_linit, cv_lsetup, cv_lsolve,
 cv_lfree fields in (*cvode_mem) to be WrapLinearCVSolveInit,
 WrapLinearCVSolveSetup, WrapLinearCVSolve, and WrapLinearCVSolveFree, respectively.
---------------------------------------------------------------*/
static int MFEMLinearCVSolve(void *ode_mem, mfem::Solver* solve,
                             mfem::SundialsLinearSolveOperator* op)
{
   CVodeMem cv_mem;

   MFEM_VERIFY(ode_mem != NULL, "CVODE memory error!");
   cv_mem = (CVodeMem) ode_mem;

   if (cv_mem->cv_lfree != NULL) { cv_mem->cv_lfree(cv_mem); }

   // Set four main function fields in ark_mem
   cv_mem->cv_linit  = WrapLinearCVSolveInit;
   cv_mem->cv_lsetup = WrapLinearCVSolveSetup;
   cv_mem->cv_lsolve = WrapLinearCVSolve;
   cv_mem->cv_lfree  = WrapLinearCVSolveFree;
   cv_mem->cv_setupNonNull = 1;
   // forces cvode to call lsetup prior to every time it calls lsolve
   cv_mem->cv_maxcor = 1;

   //void* for containing linear solver memory
   mfem::MFEMLinearSolverMemory* lmem = new mfem::MFEMLinearSolverMemory();

#ifndef MFEM_USE_MPI
   lmem->setup_y = new mfem::Vector();
   lmem->setup_f = new mfem::Vector();
   lmem->solve_y = new mfem::Vector();
   lmem->solve_yn = new mfem::Vector();
   lmem->solve_f = new mfem::Vector();
   lmem->solve_b = new mfem::Vector();
   lmem->vec_tmp = new mfem::Vector(NV_LENGTH_S(cv_mem->cv_zn[0]));
#else
   lmem->setup_y = new mfem::HypreParVector(NV_HYPRE_PARVEC_PH(
                                                cv_mem->cv_zn[0]));
   lmem->setup_f = new mfem::HypreParVector((NV_HYPRE_PARVEC_PH(
                                                cv_mem->cv_zn[0])));
   lmem->solve_y = new mfem::HypreParVector((NV_HYPRE_PARVEC_PH(
                                                cv_mem->cv_zn[0])));
   lmem->solve_f = new mfem::HypreParVector((NV_HYPRE_PARVEC_PH(
                                                cv_mem->cv_zn[0])));
   lmem->solve_b = new mfem::HypreParVector((NV_HYPRE_PARVEC_PH(
                                                cv_mem->cv_zn[0])));
   lmem->vec_tmp = new mfem::HypreParVector((NV_HYPRE_PARVEC_PH(
                                                cv_mem->cv_zn[0])));
#endif

   lmem->J_solve = solve;
   lmem->op_for_gradient = op;

   cv_mem->cv_lmem = lmem;
   return (CVSPILS_SUCCESS);
}

/*
 The purpose of ark_linit is to complete initializations for a
 specific linear solver, such as counters and statistics.
 An LInitFn should return 0 if it has successfully initialized
 the ARKODE linear solver and a negative value otherwise.
 If an error does occur, an appropriate message should be sent
 to the error handler function.
 */
static int WrapLinearARKSolveInit(ARKodeMem ark_mem)
{
   return 0;
}

/*
The job of ark_lsetup is to prepare the linear solver for
 subsequent calls to ark_lsolve. It may recompute Jacobian-
 related data as it deems necessary. Its parameters are as
 follows:

 ark_mem - problem memory pointer of type ARKodeMem. See the
          typedef earlier in this file.

 convfail - a flag to indicate any problem that occurred during
            the solution of the nonlinear equation on the
            current time step for which the linear solver is
            being used. This flag can be used to help decide
            whether the Jacobian data kept by a ARKODE linear
            solver needs to be updated or not.
            Its possible values have been documented above.

 ypred - the predicted y vector for the current ARKODE internal
         step.

 fpred - f(tn, ypred).

 jcurPtr - a pointer to a boolean to be filled in by ark_lsetup.
           The function should set *jcurPtr=TRUE if its Jacobian
           data is current after the call and should set
           *jcurPtr=FALSE if its Jacobian data is not current.
           Note: If ark_lsetup calls for re-evaluation of
           Jacobian data (based on convfail and ARKODE state
           data), it should return *jcurPtr=TRUE always;
           otherwise an infinite loop can result.

 vtemp1 - temporary N_Vector provided for use by ark_lsetup.

 vtemp3 - temporary N_Vector provided for use by ark_lsetup.

 vtemp3 - temporary N_Vector provided for use by ark_lsetup.

 The ark_lsetup routine should return 0 if successful, a positive
 value for a recoverable error, and a negative value for an
 unrecoverable error.
 */
//ypred is the predicted y at the current time, fpred is f(t,ypred)
static int WrapLinearARKSolveSetup(ARKodeMem ark_mem, int convfail,
                                   N_Vector ypred, N_Vector fpred,
                                   booleantype *jcurPtr, N_Vector vtemp1,
                                   N_Vector vtemp2, N_Vector vtemp3)
{
   mfem::MFEMLinearSolverMemory* lmem= (mfem::MFEMLinearSolverMemory*)
                                       ark_mem->ark_lmem;

#ifndef MFEM_USE_MPI
   lmem->setup_y->SetDataAndSize(NV_DATA_S(ypred),NV_LENGTH_S(ypred));
   lmem->setup_f->SetDataAndSize(NV_DATA_S(fpred),NV_LENGTH_S(fpred));
#else
   lmem->setup_y->SetData(NV_DATA_PH(ypred));
   lmem->setup_f->SetData(NV_DATA_PH(fpred));
#endif
   *jcurPtr=TRUE;
   ark_mem->ark_lmem = lmem;
   WrapLinearSolveSetup(ark_mem->ark_lmem, ark_mem->ark_tn, lmem->setup_y,
                        lmem->setup_f);
   return 0;
}

/*
 ark_lsolve must solve the linear equation P x = b, where
 P is some approximation to (M - gamma J), M is the system mass
 matrix, J = (df/dy)(tn,ycur), and the RHS vector b is input. The
 N-vector ycur contains the solver's current approximation to
 y(tn) and the vector fcur contains the N_Vector f(tn,ycur). The
 solution is to be returned in the vector b. ark_lsolve returns
 a positive value for a recoverable error and a negative value
 for an unrecoverable error. Success is indicated by a 0 return
 value.
*/
static int WrapLinearARKSolve(ARKodeMem ark_mem, N_Vector b,
                              N_Vector weight, N_Vector ycur,
                              N_Vector fcur)
{
   if (ark_mem->ark_tn>0)
   {
      mfem::MFEMLinearSolverMemory* lmem= (mfem::MFEMLinearSolverMemory*)
                                          ark_mem->ark_lmem;

#ifndef MFEM_USE_MPI
      lmem->solve_y->SetDataAndSize(NV_DATA_S(ycur),NV_LENGTH_S(ycur));
      lmem->solve_yn->SetDataAndSize(NV_DATA_S(ark_mem->ark_y),NV_LENGTH_S(ycur));
      lmem->solve_f->SetDataAndSize(NV_DATA_S(fcur),NV_LENGTH_S(fcur));
      lmem->solve_b->SetDataAndSize(NV_DATA_S(b),NV_LENGTH_S(b));
#else
      ((mfem::HypreParVector*) lmem->solve_y)->SetDataAndSize(NV_DATA_PH(ycur),
                                                              NV_LOCLENGTH_PH(ycur));
      ((mfem::HypreParVector*) lmem->solve_f)->SetDataAndSize(NV_DATA_PH(fcur),
                                                              NV_LOCLENGTH_PH(fcur));
      ((mfem::HypreParVector*) lmem->solve_b)->SetDataAndSize(NV_DATA_PH(b),
                                                              NV_LOCLENGTH_PH(b));
#endif

      lmem->weight = ark_mem->ark_gamma;

      ark_mem->ark_lmem = lmem;
      WrapLinearSolve(ark_mem->ark_lmem, ark_mem->ark_tn, lmem->solve_b,
                      lmem->solve_y,
                      lmem->setup_y, lmem->solve_f);
   }
   return 0;
}

/*
 ark_lfree should free up any memory allocated by the linear
 solver. This routine is called once a problem has been
 completed and the linear solver is no longer needed.
 */
static void WrapLinearARKSolveFree(ARKodeMem ark_mem)
{
   return;
}

/*---------------------------------------------------------------
 MFEMLinearARKSolve:

 This routine initializes the memory record and sets various
 function fields specific to the linear solver module.
 MFEMLinearARKSolve first calls the existing lfree routine if this is not
 NULL. It then sets the ark_linit, ark_lsetup, ark_lsolve,
 ark_lfree fields in (*arkode_mem) to be WrapLinearARKSolveInit,
 WrapLinearARKSolveSetup, WrapARKLinearSolve, and WrapLinearARKSolveFree,
 respectively.
---------------------------------------------------------------*/
static int MFEMLinearARKSolve(void *arkode_mem, mfem::Solver* solve,
                              mfem::SundialsLinearSolveOperator* op)
{
   ARKodeMem ark_mem;

   MFEM_VERIFY(arkode_mem != NULL, "ARKODE memory error!");
   ark_mem = (ARKodeMem) arkode_mem;

   if (ark_mem->ark_lfree != NULL) { ark_mem->ark_lfree(ark_mem); }

   // Set four main function fields in ark_mem
   ark_mem->ark_linit  = WrapLinearARKSolveInit;
   ark_mem->ark_lsetup = WrapLinearARKSolveSetup;
   ark_mem->ark_lsolve = WrapLinearARKSolve;
   ark_mem->ark_lfree  = WrapLinearARKSolveFree;
   ark_mem->ark_lsolve_type = 0;
   ark_mem->ark_linear = TRUE;
   ark_mem->ark_setupNonNull = 1;
   // forces arkode to call lsetup prior to every time it calls lsolve
   ark_mem->ark_msbp = 0;

   //void* for containing linear solver memory
   mfem::MFEMLinearSolverMemory* lmem = new mfem::MFEMLinearSolverMemory();
#ifndef MFEM_USE_MPI
   lmem->setup_y = new mfem::Vector();
   lmem->setup_f = new mfem::Vector();
   lmem->solve_y = new mfem::Vector();
   lmem->solve_yn = new mfem::Vector();
   lmem->solve_f = new mfem::Vector();
   lmem->solve_b = new mfem::Vector();
   lmem->vec_tmp = new mfem::Vector();
#else
   lmem->setup_y = new mfem::HypreParVector((NV_HYPRE_PARVEC_PH(
                                                ark_mem->ark_ycur)));
   lmem->setup_f = new mfem::HypreParVector((NV_HYPRE_PARVEC_PH(
                                                ark_mem->ark_ycur)));
   lmem->solve_y = new mfem::HypreParVector((NV_HYPRE_PARVEC_PH(
                                                ark_mem->ark_ycur)));
   lmem->solve_f = new mfem::HypreParVector((NV_HYPRE_PARVEC_PH(
                                                ark_mem->ark_ycur)));
   lmem->solve_b = new mfem::HypreParVector((NV_HYPRE_PARVEC_PH(
                                                ark_mem->ark_ycur)));
   lmem->vec_tmp = new mfem::HypreParVector((NV_HYPRE_PARVEC_PH(
                                                ark_mem->ark_ycur)));
#endif
   lmem->J_solve=solve;
   lmem->op_for_gradient= op;

   ark_mem->ark_lmem = lmem;
   return (ARKSPILS_SUCCESS);
}

#endif
