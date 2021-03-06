/* PIPS-IPM                                                           *
 * Author:  Cosmin G. Petra                                           *
 * (C) 2012 Argonne National Laboratory. See Copyright Notification.  */
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include "rawInput.hpp"
#include "PIPSIpmInterface.h"

#include "sFactoryAugSchurLeaf.h"
#include "MehrotraStochSolver.h"

#include <string>
#include <sstream>

using namespace std;


int main(int argc, char ** argv) {
  MPI_Init(&argc, &argv);
  int mype; MPI_Comm_rank(MPI_COMM_WORLD,&mype);

  if(argc<6) {
    //if (mype == 0) printf("Usage: %s [rawdump root name] [num scenarios] [solution output root name] [eq mult root name]\n",argv[0]);
    if (mype == 0) printf("Usage: %s [rawdump batches directory] [rawdump root name] [num batches] [num scenarios per batch] [output dir]\n", argv[0]);
    return 1;
  }

  if(mype==0) cout << argv[0] << " starting ..." << endl;
  
  string datadirname(argv[1]);
  string datarootname(argv[2]);
  int nbatch = atoi(argv[3]);
  int nscen = atoi(argv[4]);
  string outputdir(argv[5]);

  int nprocs; MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
  if(nprocs<nbatch) {
    if(mype == 0) cout << "Number of procs has to be greater or equal to number of batches" << endl;
    return 1;
  }

  if( nbatch* (nprocs/nbatch) != nprocs ) {
    if(mype == 0) cout << "Number of procs [" << nprocs << "] has to be multiple of the number of batches[" << nprocs/nbatch << "]" << endl;
    return 1;
  }

  if( (nprocs/nbatch) * (nscen/(nprocs/nbatch)) != nscen ) {
    if(mype == 0) cout << "Number of scens [" << nscen << "] has to be a multiple of the number of processes per batch [" << nprocs/nbatch << "]" << endl;
    return 1;
  }
  int nbatchprocs=nprocs/nbatch;
  int color = mype/nbatchprocs; 
  
  MPI_Comm commBatch;
  MPI_Comm_split(MPI_COMM_WORLD, color, 0, &commBatch);
  assert(commBatch!=MPI_COMM_NULL);

  int mynewpe; MPI_Comm_rank(commBatch, &mynewpe);

  stringstream ss; ss << datadirname << (color+1) << "/" << datarootname;
  datarootname=ss.str();
  //printf("mype=[%d][%d] datarootname=%s nscen=%d\n", mype, mynewpe, datarootname.c_str(), nscen);

  rawInput* s = new rawInput(datarootname,nscen, commBatch);

  PIPSIpmInterface<sFactoryAug, MehrotraStochSolver> pipsIpm(*s, commBatch);
  delete s;
  pipsIpm.go();

  double totalObjective=pipsIpm.getObjective();
  double stg1Objective=pipsIpm.getFirstStageObjective();
  if(mynewpe==0)
    printf("mype=[%d][%d] batch %d 1stStageObjective=%g TotalObjective=%g\n",
	   mype, mynewpe, color+1, stg1Objective, totalObjective);

  for(int s=0; s<nscen; s++) {

    std::vector<double> duals = pipsIpm.getSecondStageDualRowSolution(s);
    if(duals.size()) {
      sleep(mynewpe);
      usleep(s*50000);
      stringstream ss1; ss1<<outputdir << "/batch-" << (1+color) << "-out_duals_scen"<<(s+1)<<".txt";
      cout << "pe[" << mype << "][" << mynewpe << "] " 
	   << " saving duals to " << ss1.str() << endl;
      ofstream fileduals(ss1.str().c_str());
      
      for(size_t i=0; i<duals.size(); i++)
	fileduals << duals[i] << endl;
      fileduals.close();
    }
    
    std::vector<double> primals = pipsIpm.getSecondStagePrimalColSolution(s);
    if(primals.size()) {
	sleep(nbatchprocs-mynewpe);
      usleep( (nscen-s)*200000);
      stringstream ss2; ss2<<outputdir << "/batch-" << (1+color) << "-out_primals_scen"<<(s+1)<<".txt";
      cout << "pe[" << mype << "][" << mynewpe << "] "
	   << " saving primals to " << ss2.str() << endl;
      ofstream fileprimals(ss2.str().c_str());
      std::vector<double> primals = pipsIpm.getSecondStagePrimalColSolution(s);
      for(size_t i=0; i<primals.size(); i++)
	fileprimals << primals[i] << endl;
      fileprimals.close();
    }
  }
  if(mynewpe==0) {
    {
	std::vector<double> firstStageSol = pipsIpm.getFirstStagePrimalColSolution();
	stringstream ss2; ss2<<outputdir << "/batch-" << (1+color) << "-out_primal_1stStage.txt";
	string sFile=ss2.str();
	cout << "saving 1st stage primals to " << sFile << endl;
	ofstream file1stStg(sFile.c_str());
	for(size_t i=0; i<firstStageSol.size(); i++)
	    file1stStg << firstStageSol[i] << endl;
	file1stStg.close();
    }
    {
	std::vector<double> fstStageDuals = pipsIpm.getFirstStageDualRowSolution();
	stringstream ss2; ss2<<outputdir << "/batch-" << (1+color) << "-out_dual_1stStage.txt";
	string sFile=ss2.str();
	cout << "saving 1st stage duals to " << sFile << endl;
	ofstream file1stStg(sFile.c_str());
	for(size_t i=0; i<fstStageDuals.size(); i++)
	    file1stStg << fstStageDuals[i] << endl;
	file1stStg.close();
    }
  } else {
      sleep(10);
  }
  //cout << "pe[" << mype << "][" << mynewpe << "] "
  //     << "Solution saved, returning..." << endl;
 
  MPI_Finalize();
  return 0;
}

