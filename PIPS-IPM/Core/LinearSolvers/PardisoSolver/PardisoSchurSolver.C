/* PIPS-IPM                                                             
 * Authors: Cosmin G. Petra, Miles Lubin
 * (C) 2012 Argonne National Laboratory, see documentation for copyright
 */
#include <stdlib.h>
#include <iostream>
using namespace std;
#include <unistd.h>

#include "PardisoSchurSolver.h"
#include "SparseStorage.h"
#include "SparseSymMatrix.h"
#include "SimpleVector.h"
#include "SimpleVectorHandle.h"
#include "DenseGenMatrix.h"
#include <cstdlib>
#include <cmath>

#include "Ma57Solver.h"

#include "mpi.h"
#include "omp.h"

#ifdef HAVE_GETRUSAGE
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

extern int gOoqpPrintLevel;
extern double g_iterNumber;
extern int gOuterBiCGIter;
#ifdef STOCH_TESTING
extern double g_scenNum;
#endif
static int rhsCount=0;
using namespace std;

extern "C" void pardisoinit (void   *, int    *,   int *, int *, double *, int *);
extern "C" void pardiso     (void   *, int    *,   int *, int *,    int *, int *, 
                  double *, int    *,    int *, int *,   int *, int *,
                     int *, double *, double *, int *, double *);
extern "C" void pardiso_schur(void*, int*, int*, int*, double*, int*, int*);

extern "C" void pardiso_chkmatrix  (int *, int *, double *, int *, int *, int *);
extern "C" void pardiso_chkvec     (int *, int *, double *, int *);
extern "C" void pardiso_printstats (int *, int *, double *, int *, int *, int *,
                           double *, int *);

int pardiso_stuff(int n, int nnz, int n0, int* rowptr, int* colidx, double* elts, const vector< vector<double> >& vRhs);

int dumpAugMatrix(int n, int nnz, int nSys, //size, nnz and size of the (1,1) block
		  double* eltsA, 
		  int* rowptr, 
		  int* colidx, 
		  const char* fname=NULL);
int dumpSysMatrix(SparseSymMatrix* Msys,
                  const char* fname=NULL);
int dumpRhs(SimpleVector& v);


#ifdef TIMING_FLOPS
extern "C" {
    void HPM_Init(void);
    void HPM_Start(char *);
    void HPM_Stop(char *);
    void HPM_Print(void);
    void HPM_Print_Flops(void);
    void HPM_Print_Flops_Agg(void);
    void HPM_Terminate(char*);
}
#endif


PardisoSchurSolver::PardisoSchurSolver( SparseSymMatrix * sgm )
{
  Msys = sgm;
  n = -1;
  nvec=NULL;
  // - we do not have the augmented system yet; most of intialization done during the 
  // first solve call

  first = true; firstSolve = true;
   
  /* Numbers of processors, value of OMP_NUM_THREADS */
  char *var = getenv("OMP_NUM_THREADS");
  if(var != NULL) {
    sscanf( var, "%d", &num_threads );
  }
  else {
    printf("Set environment OMP_NUM_THREADS");
    exit(1);
  }
}

PardisoSchur32Solver::PardisoSchur32Solver( SparseSymMatrix * sgm )
    : PardisoSchurSolver( sgm )
{}

void PardisoSchurSolver::firstCall()
{
  int solver=0, mtype=-2, error;
  pardisoinit (pt,  &mtype, &solver, iparm, dparm, &error); 
  if (error!=0) {
    cout << "PardisoSchurSolver ERROR during pardisoinit:" << error << "." << endl;
    exit(1);
  }
} 

void PardisoSchur32Solver::firstCall()
{
  int solver=0, mtype=-2, error;
  pardisoinit (pt,  &mtype, &solver, iparm, dparm, &error); 
  if (error!=0) {
    cout << "PardisoSchur32Solver ERROR during pardisoinit:" << error << "." << endl;
    exit(1);
  }
  iparm[28]=1; //32-bit factorization
} 

// this function is called only once and creates the augmented system
void PardisoSchurSolver::firstSolveCall(SparseGenMatrix& R, 
					SparseGenMatrix& A,
					SparseGenMatrix& C)
{
  int nR,nA,nC;
  nnz=0;
  R.getSize(nR,nSC); nnz += R.numberOfNonZeros();
  A.getSize(nA,nSC); nnz += A.numberOfNonZeros();
  C.getSize(nC,nSC); nnz += C.numberOfNonZeros();
  int Msize=Msys->size();

  //cout << "nR=" << nR << " nA=" << nA << " nC=" << nC << " nSC=" << nSC << " sizeKi=" << (nR+nA+nC)<< endl 
  //     << "nnzR=" << R.numberOfNonZeros() 
  //     << " nnzA=" << A.numberOfNonZeros()  
  //     << " nnzC=" << C.numberOfNonZeros() << endl;

  n = nR+nA+nC+nSC;
  assert( Msize == nR+nA+nC );
  nnz += Msys->numberOfNonZeros();
  nnz += nSC; //space for the 0 diagonal of 2x2 block

  // the lower triangular part of the augmented system in row-major
  SparseSymMatrix augSys( n, nnz);
  
  //
  //put (1,1) block in the augmented system
  //
  memcpy(augSys.getStorageRef().krowM, Msys->getStorageRef().krowM, sizeof(int)*Msize);
  memcpy(augSys.getStorageRef().jcolM, Msys->getStorageRef().jcolM, sizeof(int)*Msys->numberOfNonZeros());
  memcpy(augSys.getStorageRef().M,     Msys->getStorageRef().M,     sizeof(double)*Msys->numberOfNonZeros());

  //
  //put C block in the augmented system as C^T in the lower triangular part
  //
  if(C.numberOfNonZeros()>0 && nC>0) {
    int nnzIt=Msys->numberOfNonZeros();
    
    SparseGenMatrix Ct(nSC,nC,C.numberOfNonZeros());
    int* krowCt=Ct.getStorageRef().krowM;
    int* jcolCt=Ct.getStorageRef().jcolM;
    double *MCt=Ct.getStorageRef().M;
    
    C.getStorageRef().transpose(krowCt, jcolCt, MCt);
    
    int colShift=nR+nA;
    
    int* krowAug= augSys.getStorageRef().krowM;
    int* jcolAug = augSys.getStorageRef().jcolM;
    double* MAug = augSys.getStorageRef().M;
    
    int row=Msize;
    for(; row<n; row++) {
      krowAug[row]=nnzIt;
      
      for(int c=krowCt[row-Msize]; c< krowCt[row-Msize+1]; c++) {
	
	int j=jcolCt[c];
	  
	jcolAug[nnzIt]=j+colShift;
	MAug[nnzIt]   =MCt[c];
	nnzIt++;
      }
      //add the zero from the diagonal
      jcolAug[nnzIt]=row;
      MAug[nnzIt]=0.0;
      nnzIt++;
      
    }
    krowAug[row]=nnzIt;
  }

  // -- to do
  //put R block in the augmented system as R^T in the lower triangular part
  assert(R.numberOfNonZeros()==0);
  //
  if(A.numberOfNonZeros()>0 && nA>0) {
    int nnzIt=Msys->numberOfNonZeros();
    
    SparseGenMatrix At(nSC,nA,A.numberOfNonZeros());
    int* krowAt=At.getStorageRef().krowM;
    int* jcolAt=At.getStorageRef().jcolM;
    double *MAt=At.getStorageRef().M;
    
    A.getStorageRef().transpose(krowAt, jcolAt, MAt);
    
    int colShift=nR;
    
    int* krowAug= augSys.getStorageRef().krowM;
    int* jcolAug = augSys.getStorageRef().jcolM;
    double* MAug = augSys.getStorageRef().M;
    
    int row=Msize;
    for(; row<n; row++) {
      krowAug[row]=nnzIt;
      
      for(int c=krowAt[row-Msize]; c< krowAt[row-Msize+1]; c++) {
	
	int j=jcolAt[c];
	  
	jcolAug[nnzIt]=j+colShift;
	MAug[nnzIt]   =MAt[c];
	nnzIt++;
      }
      //add the zero from the diagonal
      jcolAug[nnzIt]=row;
      MAug[nnzIt]=0.0;
      nnzIt++;
    }
    krowAug[row]=nnzIt;
  }

  nnz=augSys.numberOfNonZeros();
  // we need to transpose to get the augmented system in the row-major upper triangular format of  PARDISO 
  rowptrAug = new int[n+1];
  colidxAug = new int[nnz];
  eltsAug   = new double[nnz];

  augSys.getStorageRef().transpose(rowptrAug,colidxAug,eltsAug);
      

  //save the indeces for diagonal entries for a streamlined later update
  int* krowMsys = Msys->getStorageRef().krowM;
  int* jcolMsys = Msys->getStorageRef().jcolM;
  for(int r=0; r<Msize; r++) {
    // Msys - find the index in jcol for the diagonal (r,r)
    int idxDiagMsys=-1;
    for(int idx=krowMsys[r]; idx<krowMsys[r+1]; idx++)
      if(jcolMsys[idx]==r) {idxDiagMsys=idx; break;}
    assert(idxDiagMsys>=0);

    // aug  - find the index in jcol for the diagonal (r,r)
    int idxDiagAug=-1;
    for(int idx=rowptrAug[r]; idx<rowptrAug[r+1]; idx++)
      if(colidxAug[idx]==r) {idxDiagAug=idx; break;}
    assert(idxDiagAug>=0);

    diagMap.insert( pair<int,int>(idxDiagMsys,idxDiagAug) );
  }

  //convert to Fortran indexing
  for(int it=0; it<n+1; it++)   rowptrAug[it]++;
  for(int it=0; it<nnz; it++)   colidxAug[it]++;

  //allocate temp vector(s)
  nvec=new double[n];

  /*  //
  // symbolic analysis
  //
  int mtype=-2, error;
  int phase=11; //analysis
  int maxfct=1, mnum=1, nrhs=1;
  iparm[2]=num_threads;
  iparm[7]=8;     //# iterative refinements
  //iparm[1] = 2; // 2 is for metis, 0 for min degree 
  //iparm[ 9] =10; // pivot perturbation 10^{-xxx} 
  iparm[10] = 1; // scaling for IPM KKT; used with IPARM(13)=1 or 2
  iparm[12] = 2; // improved accuracy for IPM KKT; used with IPARM(11)=1; 
                 // if needed, use 2 for advanced matchings and higer accuracy.
  iparm[23] = 1; //Parallel Numerical Factorization (0=used in the last years, 1=two-level scheduling)

  int msglvl=0;  // with statistical information
  iparm[32] = 1; // compute determinant
  iparm[37] = Msys->size(); //compute Schur-complement

  pardiso (pt , &maxfct , &mnum, &mtype, &phase,
	   &n, eltsAug, rowptrAug, colidxAug, 
	   NULL, &nrhs,
	   iparm , &msglvl, NULL, NULL, &error, dparm );

  if ( error != 0) {
    printf ("PardisoSolver - ERROR during symbolic factorization: %d\n", error );
    assert(false);
  }
  */
} 

 
void PardisoSchurSolver::diagonalChanged( int /* idiag */, int /* extent */ )
{
  this->matrixChanged();
}

void PardisoSchurSolver::matrixChanged()
{
  if(first) { firstCall(); first=false;}

  // we don't have the right hand-size, therefore we can't (re)factorize 
  // the augmented system at this point.

  //dumpSysMatrix(Msys);
}
 
void PardisoSchurSolver::schur_solve(SparseGenMatrix& R, 
				     SparseGenMatrix& A,
				     SparseGenMatrix& C,
				     DenseSymMatrix& SC0)
{
  bool doSymbFact=false;
  if(firstSolve) { 
    firstSolveCall(R,A,C); firstSolve=false; 
    doSymbFact=true;
  } else {

    //update diagonal entries in the PARDISO aug sys
    double* eltsMsys = Msys->getStorageRef().M;
    map<int,int>::iterator it;
    for(it=diagMap.begin(); it!=diagMap.end(); it++)
      eltsAug[it->second] = eltsMsys[it->first];
  }

  // call PARDISO
  int mtype=-2, error;
  //pardiso_chkmatrix(&mtype,&n, eltsAug, rowptrAug, colidxAug, &error);
  //if(error != 0) {
  //  cout << "PARDISO check matrix error" << error << endl;
  //  exit(1);
  //}

  int nIter=(int)g_iterNumber;
  const int symbEvery=5;
  if( (nIter/symbEvery)*symbEvery==nIter )
    doSymbFact=true;

  int phase=22; // Numerical factorization
  if(doSymbFact) {
    phase =12;    //Numerical factorization & symb analysis
  } 

  int maxfct=1, mnum=1, nrhs=1;
  iparm[2]=num_threads;
  iparm[7]=8;     //# iterative refinements
  iparm[1] = 2; // 2 is for metis, 0 for min degree 
  //iparm[1] = 0; // 2 is for metis, 0 for min degree 
  //iparm[ 9] = 10; // pivot perturbation 10^{-xxx} 
  iparm[10] = 1; // scaling for IPM KKT; used with IPARM(13)=1 or 2
  iparm[12] = 2; // improved accuracy for IPM KKT; used with IPARM(11)=1; 
  // if needed, use 2 for advanced matchings and higer accuracy.
  iparm[23] = 0; //Parallel Numerical Factorization (0=used in the last years, 1=two-level scheduling)
  iparm[23] = 0; //Parallel Numerical Factorization (0=used in the last years, 1=two-level scheduling)
  iparm[24] = 0; //Parallel Numerical Factorization (0=used in the last years, 1=two-level scheduling)
  //iparm[27] = 1; // Parallel metis
  
  int msglvl=0;  // with statistical information
  //int myRankp; MPI_Comm_rank(MPI_COMM_WORLD, &myRankp);
  //if (myRankp==0) msglvl=1;
  iparm[32] = 1; // compute determinant
  iparm[37] = Msys->size(); //compute Schur-complement
  
#ifdef TIMING
  //dumpAugMatrix(n,nnz,iparm[37], eltsAug, rowptrAug, colidxAug);
  //double o=MPI_Wtime();
#endif
#ifdef TIMING_FLOPS
  HPM_Start("PARDISOFact");
#endif
  pardiso (pt , &maxfct , &mnum, &mtype, &phase,
	   &n, eltsAug, rowptrAug, colidxAug, 
	   NULL, &nrhs,
	   iparm , &msglvl, NULL, NULL, &error, dparm );
#ifdef TIMING_FLOPS
  HPM_Stop("PARDISOFact");
#endif
  int nnzSC=iparm[38];
  
#ifdef TIMING
  int myRank; MPI_Comm_rank(MPI_COMM_WORLD, &myRank);
  if(1001*(myRank/1001)==myRank)
    printf("rank %d perturbPiv %d peakmem %d\n", myRank, iparm[13], iparm[14]);
  //cout << "NNZ(SCHUR) " << nnzSC << "    SPARSITY " << nnzSC/(1.0*nSC*nSC) << endl;
#endif
  if ( error != 0) {
    printf ("PardisoSolver - ERROR during factorization: %d. Phase param=%d\n", error,phase);
    assert(false);
  }
  int* rowptrSC =new int[nSC+1];
  int* colidxSC =new int[nnzSC];
  double* eltsSC=new double[nnzSC];

  pardiso_schur(pt, &maxfct, &mnum, &mtype, eltsSC, rowptrSC, colidxSC);

  //convert back to C/C++ indexing
  for(int it=0; it<nSC+1; it++) rowptrSC[it]--;
  for(int it=0; it<nnzSC; it++) colidxSC[it]--;


  for(int r=0; r<nSC; r++) {
    for(int ci=rowptrSC[r]; ci<rowptrSC[r+1]; ci++) {
      int c=colidxSC[ci];
      SC0[r][c] += eltsSC[ci];
      if(r!=c)
	SC0[c][r] += eltsSC[ci];
    }
  }

  delete[] rowptrSC; delete[] colidxSC; delete[] eltsSC;
}

static int totalSlv=0;
void PardisoSchurSolver::solve( OoqpVector& rhs_in )
{ 
  SimpleVector& rhs=dynamic_cast<SimpleVector&>(rhs_in);

  int mtype=-2, error;
  int phase=33;      //solve and iterative refinement
  int maxfct=1, mnum=1, nrhs=1;
  iparm[2]=num_threads;
  iparm[7]=8;    // # of iterative refinements
  iparm[10] = 1; // scaling for IPM KKT; used with IPARM(13)=1 or 2
  iparm[12] = 2; // improved accuracy for IPM KKT; used with IPARM(11)=1; 
                 // if needed, use 2 for advanced matchings and higer accuracy.
  iparm[23] = 0; //Parallel Numerical Factorization (0=used in the last years, 1=two-level scheduling)
  int msglvl=0;  // with statistical information
  //int myRankp; MPI_Comm_rank(MPI_COMM_WORLD, &myRankp);
  //if (myRankp==0) msglvl=1;

  
  SimpleVector x_n(n);
  SimpleVector rhs_n(n);
  int dim=rhs.length();
  memcpy(&rhs_n[0], rhs.elements(), dim*sizeof(double));
  for(int i=dim; i<n; i++) rhs_n[i]=0.0;

#ifdef TIMING_FLOPS
  HPM_Start("PARDISOSolve");
#endif

  pardiso (pt , &maxfct , &mnum, &mtype, &phase,
	   &n, eltsAug, rowptrAug, colidxAug, 
	   NULL, &nrhs,
	   iparm , &msglvl, rhs_n.elements(), x_n.elements(), &error, dparm );   

#ifdef TIMING_FLOPS
  HPM_Stop("PARDISOSolve");
#endif

  //compute residual (alternative)  
  double* tmp_resid=new double[dim];
  memcpy(tmp_resid, rhs.elements(), dim*sizeof(double));
  for(int i=0; i<dim; i++) {
    for(int p=rowptrAug[i]; p<rowptrAug[i+1]; p++) {
      int j=colidxAug[p-1]-1;
      if(j+1<=dim) {
	//r[i] = r[i] + M(i,j)*x(j)
	tmp_resid[i] -= eltsAug[p-1]*x_n[j];
	
	if(j!=i) {
	  //r[j] = r[j] + M(j,i)*x(i)
	  tmp_resid[j] -=  eltsAug[p-1]*x_n[i];
	}
      }
    }
  }
  double res_norm2=0.0, res_nrmInf=0; 
  for(int i=0; i<dim; i++) {
      res_norm2 += tmp_resid[i]*tmp_resid[i];
      if(res_nrmInf<fabs(tmp_resid[i]))
	 res_nrmInf=tmp_resid[i];
  }
  res_norm2 = sqrt(res_norm2);

  double rhsNorm=rhs.twonorm();
  if(res_norm2/rhsNorm>1e-9) {
    cout << "PardisoSchurSolve::solve big residual --- rhs.nrm=" << rhsNorm 
	 << " iter.refin.=" << iparm[6] 
	 << " rel.res.nrm2=" << res_norm2/rhsNorm
	 << " rel.res.nrmInf=" << res_nrmInf/rhsNorm 
	 << " bicgiter=" << gOuterBiCGIter<< endl;

    //if(res_norm2/rhsNorm>1e-4 && 0==gOuterBiCGIter) {
    //  int myRank; MPI_Comm_rank(MPI_COMM_WORLD, &myRank);
    //  char tmp[1024];
    //  sprintf(tmp, "augMat-r%d-i%g-s%g-b%d.dat", myRank, g_iterNumber,  g_scenNum+1, gOuterBiCGIter);
      
    //  cout << "saving matrix to " << tmp << endl;
      
    //  dumpAugMatrix(n,nnz,iparm[37], eltsAug, rowptrAug, colidxAug);
    //  dumpRhs(rhs);
    //}
  }
  delete[] tmp_resid;
  

  memcpy(&rhs[0], x_n.elements(), dim*sizeof(double));
}

void PardisoSchur32Solver::solve( OoqpVector& rhs_in )
{ 
  SimpleVector& rhs=dynamic_cast<SimpleVector&>(rhs_in);

  int mtype=-2, error;
  int phase=33;      //solve and iterative refinement
  int maxfct=1, mnum=1, nrhs=1;
  iparm[2]=num_threads;
  iparm[7]=8;    // # of iterative refinements
  iparm[10] = 1; // scaling for IPM KKT; used with IPARM(13)=1 or 2
  iparm[12] = 2; // improved accuracy for IPM KKT; used with IPARM(11)=1; 
                 // if needed, use 2 for advanced matchings and higer accuracy.
  iparm[23] = 0; //Parallel Numerical Factorization (0=used in the last years, 1=two-level scheduling)
  iparm[23] = 0; //Parallel Numerical Factorization (0=used in the last years, 1=two-level scheduling)
  iparm[24] = 0; //Parallel Numerical Factorization (0=used in the last years, 1=two-level scheduling)
  int msglvl=0;  // with statistical information
  //int myRankp; MPI_Comm_rank(MPI_COMM_WORLD, &myRankp);
  //if (myRankp==0) msglvl=1;

  
  SimpleVector x_n(n);
  SimpleVector rhs_n(n);
  int dim=rhs.length();
  memcpy(&rhs_n[0], rhs.elements(), dim*sizeof(double));
  for(int i=dim; i<n; i++) rhs_n[i]=0.0;

  //double start = MPI_Wtime();
  pardiso (pt , &maxfct , &mnum, &mtype, &phase,
	   &n, eltsAug, rowptrAug, colidxAug, 
	   NULL, &nrhs,
	   iparm , &msglvl, rhs_n.elements(), x_n.elements(), &error, dparm );   
  //cout << "---pardiso:" << MPI_Wtime()-start << endl;


  //compute residual (alternative)
  
  double* tmp_resid=new double[dim];
  memcpy(tmp_resid, rhs.elements(), dim*sizeof(double));
  for(int i=0; i<dim; i++) {
    for(int p=rowptrAug[i]; p<rowptrAug[i+1]; p++) {
      int j=colidxAug[p-1]-1;
      if(j+1<=dim) {
	//r[i] = r[i] + M(i,j)*x(j)
	tmp_resid[i] -= eltsAug[p-1]*x_n[j];
	
	if(j!=i) {
	  //r[j] = r[j] + M(j,i)*x(i)
	  tmp_resid[j] -=  eltsAug[p-1]*x_n[i];
	}
      }
    }
  }
  double res_norm2=0.0, res_nrmInf=0; 
  for(int i=0; i<dim; i++) {
      res_norm2 += tmp_resid[i]*tmp_resid[i];
      if(res_nrmInf<fabs(tmp_resid[i]))
	 res_nrmInf=tmp_resid[i];
  }
  res_norm2 = sqrt(res_norm2);

  double rhsNorm=rhs.twonorm();
  if(res_norm2/rhsNorm>1e-3) {
    cout << "PardisoSchur32Solve::solve big residual --- rhs.nrm=" << rhsNorm 
	 << " rel.res.nrm2=" << res_norm2/rhsNorm
	 << " rel.res.nrmInf=" << res_nrmInf/rhsNorm << endl;
  }
  delete[] tmp_resid;
  

  memcpy(&rhs[0], x_n.elements(), dim*sizeof(double));
}



// void PardisoSchurSolver::solve( OoqpVector& rhs_in )
// { 
//   SimpleVector& rhs=dynamic_cast<SimpleVector&>(rhs_in);

//   dumpRhs(rhs);

//   int mtype=-2, error;
//   int phase=33;      //solve and iterative refinement
//   int maxfct=1, mnum=1, nrhs=1;
//   iparm[2]=num_threads;
//   //iparm[5]=1;    //replace rhs with sol 
//   iparm[7]=0;    // # of iterative refinements
//   //iparm[1] = 2;// 2 is for metis, 0 for min degree 
//   //iparm[ 9] =12; // pivot perturbation 10^{-xxx} 
//   iparm[10] = 1; // scaling for IPM KKT; used with IPARM(13)=1 or 2
//   iparm[12] = 2; // improved accuracy for IPM KKT; used with IPARM(11)=1; 
//                  // if needed, use 2 for advanced matchings and higer accuracy.
//   iparm[23] = 1; //Parallel Numerical Factorization (0=used in the last years, 1=two-level scheduling)
//   int msglvl=0;  // with statistical information

//   int dim=rhs.length();
//   SimpleVector x(dim); x.setToZero();
//   SimpleVector dx(nvec, dim);  
//   SimpleVector b(dim); b.copyFrom(rhs);
//   double rhsNorm = b.twonorm(); double relResNorm;
//   int refinSteps=0;
//   SimpleVector rhs_n(n);

//   vector<double> histRelRes;
//   SimpleVector x_last(dim);
//   do {

//     memcpy(&rhs_n[0], rhs.elements(), dim*sizeof(double));
//     for(int i=dim; i<n; i++) rhs_n[i]=0.0;

//     double start = MPI_Wtime();
//     pardiso (pt , &maxfct , &mnum, &mtype, &phase,
// 	     &n, eltsAug, rowptrAug, colidxAug, 
// 	     NULL, &nrhs,
// 	     iparm , &msglvl, rhs_n.elements(), nvec, &error, dparm );   

//     cout << "---pardiso:" << MPI_Wtime()-start << endl;

//     if ( error != 0) {
//       printf ("PardisoSchurSolver - ERROR during single rhs: %d\n", error );
//       exit(0);
//     }
//     x.axpy(1.0,dx);
//     //residual
//     rhs.copyFrom(b);
//     Msys->mult(1.0, rhs.elements(), 1,  -1.0, x.elements(),1);
//     relResNorm = rhs.twonorm()/rhsNorm;

//      //compute residual (alternative)
//     double* tmp_resid=new double[dim];
//     memcpy(tmp_resid, b.elements(), dim*sizeof(double));
//     for(int i=0; i<dim; i++) {
//       for(int p=rowptrAug[i]; p<rowptrAug[i+1]; p++) {
// 	int j=colidxAug[p-1]-1;
// 	if(j+1<=dim) {
// 	  //r[i] = r[i] + M(i,j)*x(j)
// 	  tmp_resid[i] -= eltsAug[p-1]*x[j];
	
// 	  if(j!=i) {
// 	    //r[j] = r[j] + M(j,i)*x(i)
// 	    tmp_resid[j] -=  eltsAug[p-1]*x[i];
// 	  }
// 	}
//       }
//     }

//     double res_norm2=0.0; 
//     for(int i=0; i<dim; i++) res_norm2 += tmp_resid[i]*tmp_resid[i];
//     res_norm2 = sqrt(res_norm2);

//     cout << "--- rhs.nrm=" << rhsNorm << " rel.res.nrm=" << relResNorm 
// 	 << " rel.res.nrm=" << res_norm2/rhsNorm
// 	 << endl;
//     delete[] tmp_resid;

//     if(histRelRes.size()>0) {
//       if(histRelRes[histRelRes.size()-1] < relResNorm) {
// 	cout << "PardisoSchurSolver::residual norm increase in iter refin, last="
// 	     << histRelRes[histRelRes.size()-1] << " new=" << relResNorm 
// 	     << ". Restoring iterate..." << endl;
// 	x.copyFrom(x_last);
// 	relResNorm=histRelRes[histRelRes.size()-1];
// 	break;
//       }
//     }
//     x_last.copyFrom(x);
//     histRelRes.push_back(relResNorm);
//     if(relResNorm<1e-9) {
//       break;
//     }
//     refinSteps++;
//   } while(refinSteps<=5);
  
//   if(relResNorm>1e-8) {

//     Ma57Solver slv(Msys);
//     slv.matrixChanged();
//     SimpleVector x_ma(dim); x_ma.copyFrom(b);
//     slv.solve(x_ma);
    
//     Msys->mult(1.0, b.elements(), 1,  -1.0, x_ma.elements(),1);
//     double relResNorm_ma57 = b.twonorm()/rhsNorm;
//     //cout <<"Ma57 rel resid norm:" << relResNorm_ma57 << endl << endl;
//     if(relResNorm_ma57 < relResNorm) {
//       x.copyFrom(x_ma);
//       relResNorm=relResNorm_ma57;
//     }
//     if(relResNorm>1e-8) {

//       cout << "Pardiso iter refinements: " << refinSteps 
// 	   << "   rel resid nrm=" << relResNorm 
// 	   << ". Ma57 rel resid=" << relResNorm_ma57 << endl;
//       //cout << "Pardiso conv history:";
//       //for(size_t it=0; it<histRelRes.size(); it++)
//       //cout << histRelRes[it] << " ";
//       //cout << endl;
//       // cout <<"Ma57 rel resid norm:" << relResNorm_ma57 << endl << endl; 
//     }
//   }

//   rhs.copyFrom(x);
// }



void PardisoSchurSolver::solve(GenMatrix& rhs_in)
{
  assert(false && "Function not supported. Use PardisoSolver for this functionality.");
}

PardisoSchurSolver::~PardisoSchurSolver()
{
  int phase = -1; /* Release internal memory . */
  int mtype = -2;
  int maxfct=  1, mnum=1, nrhs=1, msglvl=0, error;
  
  pardiso (pt, &maxfct, &mnum, &mtype, &phase,
	   &n, NULL, rowptrAug, colidxAug, NULL, &nrhs,
	   iparm, &msglvl, NULL, NULL, &error, dparm );
  if ( error != 0) {
    printf ("PardisoSchurSolver - ERROR in pardiso release: %d", error ); 
  }
  
  if(rowptrAug) delete[] rowptrAug;
  if(colidxAug) delete[] colidxAug;
  if(eltsAug)   delete[] eltsAug;
  
  if(nvec) delete[] nvec;
}

#ifdef STOCH_TESTING
int dumpAugMatrix(int n, int nnz, int nSys,
		  double* elts, int* rowptr, int* colidx, 
		  const char* fname)
{
  char filename[1024];
  int myRank; MPI_Comm_rank(MPI_COMM_WORLD, &myRank);
  if(fname==NULL) 
    sprintf(filename, "augMat-r%d-i%g-s%g.dat", myRank, g_iterNumber,  g_scenNum+1);
  else 
    sprintf(filename, "%s-r%d-i%g-s%g.dat", fname, myRank, g_iterNumber,  g_scenNum+1);

  cout << "saving to:" << filename << " ...";

  ofstream fd(filename);
  fd << scientific;
  fd.precision(16);

  fd << n << endl << nSys << endl << nnz << endl;
  for(int it=0; it<n+1; it++) fd << rowptr[it] << " ";  fd << endl;
  for(int it=0; it<nnz; it++) fd << colidx[it] << " ";  fd << endl;
  for(int it=0; it<nnz; it++) fd << elts[it]   << " ";  fd << endl;

  cout << filename << " done!" << endl;
  return 0;
}

int dumpSysMatrix(SparseSymMatrix* Msys, const char* fname)
{
  char filename[1024];
  if(fname==NULL) 
    sprintf(filename, "Qdump-%g-s%g.dat", g_iterNumber,  g_scenNum+1);
  else 
    sprintf(filename, "%s-%g-s%g.dat", fname, g_iterNumber,  g_scenNum+1);

  cout << "saving to:" << filename << " ...";

  int n  =Msys->size();
  int nnz=Msys->numberOfNonZeros();

  // we need to transpose to get the augmented system in the row-major upper triangular format of  PARDISO 
  int* rowptr  = new int[n+1];
  int* colidx  = new int[nnz];
  double* elts = new double[nnz];
  Msys->getStorageRef().transpose(rowptr,colidx,elts);


  ofstream fd(filename);
  fd << scientific;
  fd.precision(16);

  fd << n << endl << nnz << endl;
  for(int it=0; it<n+1; it++) fd << rowptr[it]+1 << " ";  fd << endl;
  for(int it=0; it<nnz; it++) fd << colidx[it]+1 << " ";  fd << endl;
  for(int it=0; it<nnz; it++) fd << elts[it]   << " ";  fd << endl;

  delete[] rowptr; delete[] colidx; delete[] elts;

  cout << " Done!" << endl;
  

  return 0;
}
int dumpRhs(SimpleVector& v)
{
  rhsCount++;
  char filename[1024];
  sprintf(filename, "rhsDump-%g-%d.dat", g_iterNumber,   rhsCount);
  cout << "saving to: " << filename << " ...";

  ofstream fd(filename);
  fd << scientific;
  fd.precision(16);

  fd << v.length() << endl;
  for(int i=0; i<v.length(); i++) fd << v[i]   << " ";  
  fd << endl;

  cout << "done!" << endl;
  return 0;
}
#endif
// int dumpSysMatrix(SparseSymMatrix& M)
// {
//   int n=M.size(), nnz=M.numberOfNonZeros();

//   int*  rowptr = new int[n+1];
//   int*  colidx = new int[nnz];
//   double* elts = new double[nnz];

//   M.getStorageRef().transpose(rowptr,colidx,elts);

//   //FORTRAN indexes
//   for(int it=0; it<n+1; it++) rowptr[it]++;
//   for(int it=0; it<nnz; it++) colidx[it]++;

//   char fname[128];
//   strcpy(fname, "matDump.dat");
//   dumpAugMatrix(n, nnz, elts, rowptr, colidx, fname);
//   return 0;
// }

/*
int pardiso_stuff(int n, int nnz, int n0, int* rowptr, int* colidx, double* elts, const vector< vector<double> >& vRhs)
{
  void  *pt[64];
  int iparm[64];
  double dparm[64];
  int num_threads;

  char *var = getenv("OMP_NUM_THREADS");
  if(var != NULL) {
    sscanf( var, "%d", &num_threads );
    cout << "Using " << num_threads << " threads." << endl;
  }
  else {
    printf("Set environment OMP_NUM_THREADS\n");
    exit(1);
  }
  int solver=0, mtype=-2, error;
  pardisoinit (pt,  &mtype, &solver, iparm, dparm, &error);
  if (error!=0) {
    cout << "Pardiso solver ERROR during pardisoinit:" << error << "." << endl;
    exit(1);
  }

  pardiso_chkmatrix(&mtype, &n, elts, rowptr, colidx, &error);
  if(0!=error)
    cout << "Error in matrix detected: " << error << endl;
  //int zero=0;
  //pardiso_printstats (&mtype, &n, elts, rowptr, colidx, &zero, NULL, &error);

  //
  // symbolic analysis & numerical factorization
  //
  int phase=12; //analysis & fact
  int maxfct=1, mnum=1, nrhs=1;
  iparm[2]=num_threads;
  iparm[7]=8;     //# iterative refinements
  //iparm[1] = 2; // 2 is for metis, 0 for min degree
  //iparm[ 9] =10; // pivot perturbation 10^{-xxx}
  iparm[10] = 1; // scaling for IPM KKT; used with IPARM(13)=1 or 2
  iparm[12] = 2; // improved accuracy for IPM KKT; used with IPARM(11)=1;
                 // if needed, use 2 for advanced matchings and higer accuracy.
  iparm[23] = 1; //Parallel Numerical Factorization (0=used in the last years, 1=two-level scheduling)

  int msglvl=0;  // with statistical information
  iparm[32] = 0; // compute determinant
  if(n0!=n)  iparm[37] = n0; //compute Schur-complement

  double start=omp_get_wtime();
  pardiso (pt, &maxfct , &mnum, &mtype, &phase,
           &n, elts, rowptr, colidx,
           NULL, &nrhs,
           iparm , &msglvl, NULL, NULL, &error, dparm );
  cout << "Factorization: " << omp_get_wtime()-start << endl;
  if ( error != 0) {
    printf ("PardisoSolver - ERROR during factorization: %d\n", error );
    exit(1);
  }


  ////////////////////////////////////////////////////////////
  int nSC=n-n0;
  if(nSC>0) {
    int nnzSC=iparm[38];
    int* rowptrSC =new int[nSC+1];
    int* colidxSC =new int[nnzSC];
    double* eltsSC=new double[nnzSC];
    
    pardiso_schur(pt, &maxfct, &mnum, &mtype, eltsSC, rowptrSC, colidxSC);
    //!cout << "Schur complement (2nd stage) n=" << nSC << "   nnz=" << nnzSC << endl;
    
    //convert back to C/C++ indexing
    for(int it=0; it<nSC+1; it++) rowptrSC[it]--;
    for(int it=0; it<nnzSC; it++) colidxSC[it]--;
    
    
    //for(int r=0; r<nSC; r++) {
    //  for(int ci=rowptrSC[r]; ci<rowptrSC[r+1]; ci++) {
    //	int c=colidxSC[ci];
    //	SC0[r][c] += eltsSC[ci];
    //	if(r!=c)
    //	  SC0[c][r] += eltsSC[ci];
    //}
    //}
    delete[] rowptrSC; delete[] colidxSC; delete[] eltsSC;
  }

  



  ////////////////////////////////////////////////////////////


  double* rhs=new double[n];
  double* sol=new double[n];
  double* resid=new double[n0];
  //solve with each rhs
  for(int r=0; r<vRhs.size(); r++) {
    memcpy(rhs,   &vRhs[r][0], n0*sizeof(double));
    memcpy(resid, &vRhs[r][0], n0*sizeof(double));


    //
    // solve
    //
    phase=33;
    iparm[7]=8;    // # of iterative refinements
    int nrhs=1;
    msglvl=0;

    start = omp_get_wtime();
    pardiso (pt , &maxfct , &mnum, &mtype, &phase,
	     &n, elts, rowptr, colidx, 
	     NULL, &nrhs,
	     iparm , &msglvl, rhs, sol, &error, dparm );   
    double t=omp_get_wtime()-start; 

    //cout << "Sol:";
    //for(int i=2000; i<2003; i++) cout << sol[i] << " ";
    //cout << endl;

    //compute residual
    for(int i=0; i<n0; i++) {
      for(int p=rowptr[i]; p<rowptr[i+1]; p++) {
	int j=colidx[p-1]-1;
	if(j+1<=n0) {
	  //r[i] = r[i] + M(i,j)*x(j)
	  resid[i] -= elts[p-1]*sol[j];
	
	  if(j!=i) {
	    //r[j] = r[j] + M(j,i)*x(i)
	    resid[j] -=  elts[p-1]*sol[i];
	  }
	}
      }
    }
    double res_norm=0.0, rhs_norm=0.0;
    for(int i=0; i<n0; i++) res_norm += resid[i]*resid[i];
    for(int i=0; i<n0; i++) rhs_norm += rhs[i]*rhs[i];
    res_norm=sqrt(res_norm); rhs_norm=sqrt(rhs_norm);
    cout << "Solve: " << t << " sec."
         << " Rhs.nrm: " << rhs_norm 
	 << " Rel.resid.nrm=" << res_norm/rhs_norm
	 << endl;
  }
  delete[] rhs; delete[] sol; delete[] resid;

  //
  // clean-up
  //
  phase = -1; // Release internal memory .

  pardiso (pt, &maxfct, &mnum, &mtype, &phase,
           &n, NULL, rowptr, colidx, NULL, &nrhs,
           iparm, &msglvl, NULL, NULL, &error, dparm );
  if ( error != 0) {
    printf ("Pardiso solver - ERROR in pardiso release: %d", error );
    exit(1);
  }

  return 0; 
}
*/
