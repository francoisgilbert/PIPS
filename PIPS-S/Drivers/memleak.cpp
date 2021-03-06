#include "BAData.hpp"
#include "SMPSInput.hpp"
#include "PIPSSInterface.hpp"
#include <boost/scoped_ptr.hpp>
#include <cstdlib>
#include <vector>

//#include "BAVector.hpp"

using boost::scoped_ptr; // replace with unique_ptr for C++11
using namespace std;

int main(int argc, char **argv) {

  cout << "Start" << endl;
  MPI_Init(&argc, &argv);

  int mype;
  MPI_Comm_rank(MPI_COMM_WORLD,&mype);

  if (argc < 2) {
    if (0 == mype) printf("Usage: %s [SMPS input root name]\n", argv[0]);
    return 1;
  }

  string smpsrootname(argv[1]);

  scoped_ptr<SMPSInput> s(new SMPSInput(smpsrootname+".cor", smpsrootname+".tim", smpsrootname+".sto"));
  int nscen = s->nScenarios();

  BAContext ctx(MPI_COMM_WORLD);
  ctx.initializeAssignment(nscen);

  BADimensions dims(*s, ctx);

  cout << " ctx and dims created" << endl;

  //test for denseBAVector
  if (0) {

    vector<denseBAVector> v1,v2;

    denseBAVector baVec1Copy;

    {
      denseBAVector baVec1,baVec2;
      baVec1.allocate(dims, ctx, PrimalVector);

      v1.push_back(baVec1);

      baVec2.allocate(dims,ctx,PrimalVector);
      v1.push_back(baVec2);


      if (0 == mype) cout << "vector1 has " << v1.size() << " elements\n";
      denseBAVector baVec3 = v1[0];

      //testing assignement operator
      baVec1Copy = v1[1];

      baVec2 = baVec1;

      v2.push_back(baVec3);
      v2.push_back(baVec1Copy);
      v2.push_back(baVec2);

      // testing pop
      if (0 == mype) cout << "Pop elements from vector1\n";
      v1.pop_back();
      v1.pop_back();

    }
    denseBAVector baVec;
    baVec.allocate(dims, ctx, PrimalVector);
    v2.push_back(baVec);

    cout << "vector2 has " << v2.size() << " elements\n";

    if (0 == mype) cout << "Pop elements from vector2\n";
    v2.pop_back();
    v2.pop_back();
    v2.pop_back();
    v2.pop_back();

  }

  //test for BAFlag
  if(0) {
    vector<BAFlagVector<variableState> > v, v2;
    BAFlagVector<variableState> flagVec1, flagVec2;

    flagVec1.allocate(dims, ctx, PrimalVector);
    flagVec2=flagVec1;

    v.push_back(flagVec1);
    v.push_back(flagVec2);


    BAFlagVector<variableState> flagVec3 = flagVec2;
    BAFlagVector<variableState> flagVec4 = flagVec1;

    v.push_back(flagVec3);
    v.push_back(flagVec4);

    {
      BAFlagVector<variableState> flagVec = v[2];
      BAFlagVector<variableState> flagVec5;
      flagVec5=flagVec;
    }

    v2=v;

    v2.pop_back();
    v2.pop_back();
    v2.pop_back();
    v2.pop_back();

    v.pop_back();
    v.pop_back();
    v.pop_back();
    v.pop_back();

  }

  //test for BAContainer
  if(1) {
    BAContainer<vector<int> > dualInfeas;


    dualInfeas.allocate(dims, ctx, PrimalVector);

    dualInfeas.getFirstStageVec().push_back(-11);
    dualInfeas.getSecondStageVec(0).push_back(0);
    dualInfeas.getSecondStageVec(1).push_back(11);


    BAContainer<vector<int> > dualInfeas2 =dualInfeas;

    BAContainer<vector<int> > dualInfeas3;
    dualInfeas3 = dualInfeas2;
    cout << dualInfeas2.getVec(-1)[0] << " | " << dualInfeas2.getSecondStageVec(1)[0] << "\n";
    cout << dualInfeas3.getFirstStageVec()[0] << " | " << dualInfeas3.getSecondStageVec(1)[0] << "\n";


  }
  MPI_Finalize();
  cout << "driver exiting ..." << endl;
  return 0;
}

