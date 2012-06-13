/* PIPS-IPM                                                             
 * Authors: Cosmin G. Petra, Miles Lubin, Murat Mut
 * (C) 2012 Argonne National Laboratory, see documentation for copyright
 */
#ifndef PARDISOLINSYS_H
#define PARDISOLINSYS_H

#include "DoubleLinearSolver.h"
#include "SparseSymMatrixHandle.h"
#include "SparseStorageHandle.h"
#include "OoqpVectorHandle.h"
#include "SparseStorage.h"



#ifndef FNAME
#ifndef __bg__
#define FNAME(f) f ## _ 
#else
#define FNAME(f) f // no underscores for fortran names on bgp
#endif
#endif


/** implements the linear solver class using the Pardiso solver
 */
 
class PardisoSolver : public DoubleLinearSolver {
private:
  PardisoSolver() {};
  
 public:
  virtual void firstCall();
  
  /** sets mStorage to refer to the argument sgm */
  PardisoSolver( SparseSymMatrix * sgm ); 
  
  
  virtual void diagonalChanged( int idiag, int extent );
  virtual void matrixChanged();
  virtual void solve( OoqpVector& rhs );
  virtual void solve( GenMatrix& rhs);
  
 // virtual void Lsolve( OoqpVector& x );
 // virtual void Dsolve( OoqpVector& x );
 // virtual void Ltsolve( OoqpVector& x );
  
 private:
  SparseSymMatrix* Msys;
  bool first;
  void  *pt[64]; 
  int mtype;
  int solver;
  int iparm[64];
  int num_threads;
  
  double b[8], x[8];
  double dparm[64];
  int error;
  int nrhs;  //  Number of right-hand sides 
  int maxfct;
  int mnum;
  int phase;
  int n;

  /** storage for the upper triangular (in row-major format) */
  int     *krowM,    *jcolM;
  double  *M;
  
  
  /** number of nonzeros in the matrix */
  int      nnz;
  
  /** temporary storage for the factorization process */
  //int     *perm, *invp, *diagmap;
  double* nvec; //temporary vec
  
  virtual ~PardisoSolver();
};

#endif