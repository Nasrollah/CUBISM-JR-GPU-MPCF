/* *
 * Sim_SICCloudMPI.cpp
 *
 * Created by Fabian Wermelinger on 7/22/14.
 * Copyright 2014 ETH Zurich. All rights reserved.
 * */
#include "Sim_SICCloudMPI.h"
#include "HDF5Dumper_MPI.h"
#include "Types.h"

#include <cassert>
#include <fstream>
#include <vector>
#include <iostream>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <sstream>
#include <mpi.h>
using namespace std;

#define _BUBBLECENTIPEDE_X_ 1
#define _MPI_Y2_ 0

#ifdef _FLOAT_PRECISION_
#define _MPIREAL_ MPI_FLOAT
#else
#define _MPIREAL_ MPI_DOUBLE
#endif


namespace SICCloudData
{
    int n_shapes = 0;
    int n_small = 0;
    int small_count = 0;
    double min_rad = 0;
    double max_rad = 0;
    double seed_s[3], seed_e[3];
    int n_sensors = 0;
    Real nx, ny, nz, Sx, Sy, Sz;
    Real pressureRatio, rho0, c0, u0, p0, rhoB, uB, pB;
    Real rho1, u1, p1, mach;
    Real g1, g2, pc1, pc2;
    Real post_shock_conservative[GridMPI::NVAR];
}

void Sim_SICCloudMPI::_allocGPU()
{
    if (isroot) printf("Allocating GPUlabMPISICCloud...\n");

    // if we want to do a state update, allocate a capable lab
    bool state = false;
    if (parser("-state").asBool(false)) state = true;

    myGPU = new GPUlabMPISICCloud(*mygrid, nslices, verbosity, isroot, state);
}

void Sim_SICCloudMPI::_dump(const string basename)
{
    const string dump_path = parser("-fpath").asString(".");

    if (isroot) printf("Dumping data%04d at step %d, time %f\n", fcount, LSRK3_DataMPI::step, LSRK3_DataMPI::time);
    sprintf(fname, "%s_%04d-p", basename.c_str(), fcount);
    DumpHDF5_MPI<GridMPI, myPressureStreamer>(*mygrid, LSRK3_DataMPI::step, fname, dump_path);
    sprintf(fname, "%s_%04d-G", basename.c_str(), fcount);
    DumpHDF5_MPI<GridMPI, myGammaStreamer>(*mygrid, LSRK3_DataMPI::step, fname, dump_path);
    /* sprintf(fname, "%s_%04d-P", basename.c_str(), fcount); */
    /* DumpHDF5_MPI<GridMPI, myPiStreamer>(*mygrid, LSRK3_DataMPI::step, fname, dump_path); */
    /* sprintf(fname, "%s_%04d-rho", basename.c_str(), fcount); */
    /* DumpHDF5_MPI<GridMPI, myRhoStreamer>(*mygrid, LSRK3_DataMPI::step, fname, dump_path); */
    /* sprintf(fname, "%s_%04d-velocity", basename.c_str(), fcount); */
    /* DumpHDF5_MPI<GridMPI, myVelocityStreamer>(*mygrid, LSRK3_DataMPI::step, fname, dump_path); */
    /* sprintf(fname, "%s_%04d-energy", basename.c_str(), fcount); */
    /* DumpHDF5_MPI<GridMPI, myEnergyStreamer>(*mygrid, LSRK3_DataMPI::step, fname, dump_path); */
    ++fcount;
}

void Sim_SICCloudMPI::_dump_statistics(const int step_id, const Real t, const Real dt)
{
    double rInt=0., uInt=0., vInt=0., wInt=0., eInt=0., vol=0., ke=0., r2Int=0., mach_max=-HUGE_VAL, p_max=-HUGE_VAL, p_avg=0.;
    const double h = mygrid->getH();
    const double h3 = h*h*h;

    const Real * const r = mygrid->pdata()[0];
    const Real * const u = mygrid->pdata()[1];
    const Real * const v = mygrid->pdata()[2];
    const Real * const w = mygrid->pdata()[3];
    const Real * const e = mygrid->pdata()[4];
    const Real * const G = mygrid->pdata()[5];
    const Real * const P = mygrid->pdata()[6];

    const size_t N = mygrid->size();
#pragma omp parallel for
    for (size_t i=0; i < N; ++i)
    {
        rInt  += r[i];
        uInt  += u[i];
        vInt  += v[i];
        wInt  += w[i];
        eInt  += e[i];
        const Real G1 = 1.0 / (MaterialDictionary::gamma1 - 1.0);
        const Real G2 = 1.0 / (MaterialDictionary::gamma2 - 1.0);
        vol   += G[i] > 0.5 * (G1 + G2) ? 1.0 : 0.0;
        r2Int += r[i] * (1.0 - min(max((G[i] - static_cast<Real>(1.0/(MaterialDictionary::gamma1 - 2.0))) / (G1 - G2), static_cast<Real>(0.0)), static_cast<Real>(1.0)));
        const Real ImI2 = (u[i]*u[i] + v[i]*v[i] + w[i]*w[i]);
        ke += static_cast<Real>(0.5)/r[i] * ImI2;

        const double pressure = (e[i] - static_cast<Real>(0.5)/r[i] * ImI2 - P[i]) / G[i];
        const double c = sqrt((static_cast<Real>(1.0)/G[i] + static_cast<Real>(1.0)) * (pressure + P[i]/(G[i] + static_cast<Real>(1.0))) / r[i]);
        const double IvI = sqrt(ImI2) / r[i];

        p_avg += pressure;
        mach_max = max(mach_max, IvI/c);
        p_max    = max(p_max, pressure);
    }

#if _BUBBLECENTIPEDE_X_
    // Only if leading dimension is X !!!!

    vector<Real> mycenterline_r(_BLOCKSIZEX_);
    vector<Real> mycenterline_p(_BLOCKSIZEX_);
    vector<Real> mycenterline_u(_BLOCKSIZEX_);
    vector<Real> mycenterline_v(_BLOCKSIZEX_);
    vector<Real> mycenterline_w(_BLOCKSIZEX_);
    vector<Real> mycenterline_G(_BLOCKSIZEX_);
    vector<Real> mycenterline_P(_BLOCKSIZEX_);

    // copy the shit
    int mypeidx[3];
    mygrid->peindex(mypeidx);
#if _MPI_Y2_
    const size_t idx_start0 = ID3(0, _BLOCKSIZEY_-1, _BLOCKSIZEZ_/2, _BLOCKSIZEX_, _BLOCKSIZEY_);
    const size_t idx_start1 = ID3(0, 0,              _BLOCKSIZEZ_/2, _BLOCKSIZEX_, _BLOCKSIZEY_);
    const size_t my_idxstart = idx_start0*(1-mypeidx[1]) + idx_start1*mypeidx[1];
#else
    const size_t my_idxstart = ID3(0, _BLOCKSIZEY_/2, _BLOCKSIZEZ_/2, _BLOCKSIZEX_, _BLOCKSIZEY_);
#endif
    memcpy(&mycenterline_r.front(), &r[my_idxstart], _BLOCKSIZEX_*sizeof(Real));
    memcpy(&mycenterline_p.front(), &e[my_idxstart], _BLOCKSIZEX_*sizeof(Real));
    memcpy(&mycenterline_G.front(), &G[my_idxstart], _BLOCKSIZEX_*sizeof(Real));
    memcpy(&mycenterline_P.front(), &P[my_idxstart], _BLOCKSIZEX_*sizeof(Real));
    memcpy(&mycenterline_u.front(), &u[my_idxstart], _BLOCKSIZEX_*sizeof(Real));
    memcpy(&mycenterline_v.front(), &v[my_idxstart], _BLOCKSIZEX_*sizeof(Real));
    memcpy(&mycenterline_w.front(), &w[my_idxstart], _BLOCKSIZEX_*sizeof(Real));

    // convert to primitives
    for (int i=0; i < _BLOCKSIZEX_; ++i)
    {
        const Real ui = mycenterline_u[i] / mycenterline_r[i];
        const Real vi = mycenterline_v[i] / mycenterline_r[i];
        const Real wi = mycenterline_w[i] / mycenterline_r[i];
        const Real pi = (mycenterline_p[i] - 0.5*mycenterline_r[i]*(ui*ui + vi*vi + wi*wi) - mycenterline_P[i]) / mycenterline_G[i];
        mycenterline_u[i] = ui;
        mycenterline_v[i] = vi;
        mycenterline_w[i] = wi;
        mycenterline_p[i] = pi;
    }

    // gather the shit ONLY WORKS FOR TOPOLOGY WITH 2 PROCESSES IN Y
    int gsize;
    MPI_Comm_size(MPI_COMM_WORLD, &gsize);
    vector<Real> centerline_r(gsize * _BLOCKSIZEX_);
    vector<Real> centerline_p(gsize * _BLOCKSIZEX_);
    vector<Real> centerline_u(gsize * _BLOCKSIZEX_);
    vector<Real> centerline_v(gsize * _BLOCKSIZEX_);
    vector<Real> centerline_w(gsize * _BLOCKSIZEX_);

    const MPI_Comm cartcomm = mygrid->getCartComm();
    MPI_Gather(&mycenterline_r.front(), _BLOCKSIZEX_, _MPIREAL_, &centerline_r.front(), _BLOCKSIZEX_, _MPIREAL_, 0, cartcomm);
    MPI_Gather(&mycenterline_p.front(), _BLOCKSIZEX_, _MPIREAL_, &centerline_p.front(), _BLOCKSIZEX_, _MPIREAL_, 0, cartcomm);
    MPI_Gather(&mycenterline_u.front(), _BLOCKSIZEX_, _MPIREAL_, &centerline_u.front(), _BLOCKSIZEX_, _MPIREAL_, 0, cartcomm);
    MPI_Gather(&mycenterline_v.front(), _BLOCKSIZEX_, _MPIREAL_, &centerline_v.front(), _BLOCKSIZEX_, _MPIREAL_, 0, cartcomm);
    MPI_Gather(&mycenterline_w.front(), _BLOCKSIZEX_, _MPIREAL_, &centerline_w.front(), _BLOCKSIZEX_, _MPIREAL_, 0, cartcomm);
#endif

    // reduce MPI
    double g_rInt=0., g_uInt=0., g_vInt=0., g_wInt=0., g_eInt=0., g_vol=0., g_ke=0., g_r2Int=0., g_mach_max=-HUGE_VAL, g_p_max=-HUGE_VAL, g_p_avg=0.;
    MPI_Reduce(&rInt,     &g_rInt,     1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&uInt,     &g_uInt,     1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&vInt,     &g_vInt,     1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&wInt,     &g_wInt,     1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&eInt,     &g_eInt,     1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&vol,      &g_vol,      1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&r2Int,    &g_r2Int,    1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&ke,       &g_ke,       1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&mach_max, &g_mach_max, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
    MPI_Reduce(&p_max,    &g_p_max,    1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
    MPI_Reduce(&p_avg,    &g_p_avg,    1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);

    double extent[3];
    mygrid->get_gextent(extent);
    const double volume = extent[0] * extent[1] * extent[2];

    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    if (0 == rank)
    {
        FILE * f = fopen("integrals.dat", "a");
        fprintf(f, "%d %e %e %e %e %e %e %e %e %e %e %e %e %e %e\n",
                step_id, t, dt, g_rInt*h3, g_uInt*h3, g_vInt*h3, g_wInt*h3,
                g_eInt*h3, g_vol*h3, g_ke*h3, g_r2Int*h3, g_mach_max, g_p_max,
                pow(0.75*g_vol*h3/M_PI,1./3.), g_p_avg*h3/volume);
        fclose(f);

#if _BUBBLECENTIPEDE_X_
        char fname[256];
        sprintf(fname, "centerline_%05d.dat", step_id);
        f = fopen(fname, "w");
        fprintf(f, "# Step: %d\n", step_id);
        fprintf(f, "# Time: %e\n", t);
        fprintf(f, "# dt:   %e\n", dt);
        fprintf(f, "# h:    %e\n", h);

        Real* cr = &centerline_r.front();
        Real* cp = &centerline_p.front();
        Real* cu = &centerline_u.front();
        Real* cv = &centerline_v.front();
        Real* cw = &centerline_w.front();
#if _MPI_Y2_
        for (int j=0; j < gsize/2; ++j)
        {
            for (int i=0; i < _BLOCKSIZEX_; ++i)
                fprintf(f, "%e %e %e %e %e\n",
                        0.5*(cr[i] + cr[i+_BLOCKSIZEX_]),
                        0.5*(cp[i] + cp[i+_BLOCKSIZEX_]),
                        0.5*(cu[i] + cu[i+_BLOCKSIZEX_]),
                        0.5*(cv[i] + cv[i+_BLOCKSIZEX_]),
                        0.5*(cw[i] + cw[i+_BLOCKSIZEX_]));
            cr += 2*_BLOCKSIZEX_;
            cp += 2*_BLOCKSIZEX_;
            cu += 2*_BLOCKSIZEX_;
            cv += 2*_BLOCKSIZEX_;
            cw += 2*_BLOCKSIZEX_;
        }
#else
        for (int i=0; i < centerline_r.size(); ++i)
            fprintf(f, "%e %e %e %e %e\n", cr[i], cp[i], cu[i], cv[i], cw[i]);
#endif
        fclose(f);
#endif
    }
}

void Sim_SICCloudMPI::_dump_sensors(const int step_id, const Real t, const Real dt)
{
    /* TODO: (Thu 30 Oct 2014 02:03:34 PM CET) currently not needed */
}

void Sim_SICCloudMPI::_set_constants()
{
    /* *
     * Note that if a simulation is restarted, the same initial values should
     * be provided as arguments as if it was the first run. This is because
     * boundary conditions (such as Dirichlet inflow) depend on the initial
     * values, e.g. post shock conditions. It is up to the user to take care of
     * this situation!
     * */
    if (isroot)
    {
        printf("=====================================================================\n");
        printf("                     Shock Induced Cloud Collapse                    \n");
        printf("=====================================================================\n");
    }
    // initial values correspond to sec 4.7 of "Finite-volume WENO scheme for
    // viscous compressible multicomponent flows" V.Coralic, T.Colonius, 2014

    const Real p_scale = 1.01325e5; // atmospheric pressure scale

    // liquid
    SICCloudData::rho0 = parser("-rho0").asDouble(1000);
    SICCloudData::u0   = parser("-u0").asDouble(0);
    SICCloudData::p0   = parser("-p0").asDouble(101325/p_scale);
    // bubbles
    SICCloudData::rhoB = parser("-rhoB").asDouble(1);
    SICCloudData::uB   = parser("-uB").asDouble(0);
    SICCloudData::pB   = parser("-pB").asDouble(101325/p_scale);
    // pressure ratio over shock
    SICCloudData::pressureRatio = parser("-pressureratio").asDouble(400);

    SICCloudData::g1  = parser("-g1").asDouble(6.12);
    SICCloudData::pc1 = parser("-pc1").asDouble(3.43e8/p_scale);
    SICCloudData::g2  = parser("-g2").asDouble(1.4);
    SICCloudData::pc2 = parser("-pc2").asDouble(0.0/p_scale);
    // set in global material dictionary
    MaterialDictionary::rho1 = SICCloudData::rho0;
    MaterialDictionary::rho2 = SICCloudData::rhoB;
    MaterialDictionary::gamma1 = SICCloudData::g1;
    MaterialDictionary::gamma2 = SICCloudData::g2;
    MaterialDictionary::pc1 = SICCloudData::pc1;
    MaterialDictionary::pc2 = SICCloudData::pc2;

    /* *
     * Compute post shock states based on Appendix A of E. Johnsen "Numerical
     * Simulations of Non-Spherical Bubble Collapse", PhD Thesis, Caltech 2007.
     * */
    const Real gamma = SICCloudData::g1;
    const Real pc    = SICCloudData::pc1;
    const Real p0    = SICCloudData::p0;
    const Real rho0  = SICCloudData::rho0;
    const Real u0    = SICCloudData::u0;
    const Real psi   = SICCloudData::pressureRatio;
    const Real p1    = p0*psi;
    const Real tmp1  = (gamma + 1.0)/(gamma - 1.0);
    const Real tmp2  = (p1 + pc)/(p0 + pc);

    SICCloudData::c0   = sqrt(gamma*(p0 + pc)/rho0);
    SICCloudData::mach = sqrt((gamma+1.0)/(2.0*gamma)*(psi - 1.0)*p0/(p0 + pc) + 1.0);
    SICCloudData::u1   = u0 + SICCloudData::c0*(psi - 1.0)*p0/(gamma*(p0 + pc)*SICCloudData::mach);
    SICCloudData::rho1 = rho0*(tmp1*tmp2 + 1.0)/(tmp1 + tmp2);
    SICCloudData::p1   = p1;

    /* TODO: (Sat 11 Oct 2014 01:16:42 PM CEST) Generalize this at some point */
    // REALITY CHECK: this only works for normal shock in x-direction
    SICCloudData::post_shock_conservative[0] = SICCloudData::rho1;
    SICCloudData::post_shock_conservative[1] = SICCloudData::rho1*SICCloudData::u1;
    SICCloudData::post_shock_conservative[2] = static_cast<Real>(0.0);
    SICCloudData::post_shock_conservative[3] = static_cast<Real>(0.0);
    const Real G1 = 1.0 / (gamma - 1.0);
    SICCloudData::post_shock_conservative[4] = p1*G1 + pc*gamma*G1 + static_cast<Real>(0.5)*SICCloudData::rho1*SICCloudData::u1*SICCloudData::u1; // v = w = 0 !
    SICCloudData::post_shock_conservative[5] = G1;
    SICCloudData::post_shock_conservative[6] = pc*gamma*G1;
}

void Sim_SICCloudMPI::_ic()
{
    _set_constants(); // set case constants

    /* *
     * Initial shock orientation is only needed to set up the initial
     * condition, not for restarts
     * */

    // shock orienation
    SICCloudData::nx = parser("-shockNx").asDouble(1.0);
    SICCloudData::ny = parser("-shockNy").asDouble(0.0);
    SICCloudData::nz = parser("-shockNz").asDouble(0.0);
    // point on shock
    SICCloudData::Sx = parser("-shockSx").asDouble(0.05);
    SICCloudData::Sy = parser("-shockSy").asDouble(0.0);
    SICCloudData::Sz = parser("-shockSz").asDouble(0.0);

    // normalize shock normal vector
    const Real mag = sqrt(pow(SICCloudData::nx, 2) + pow(SICCloudData::ny, 2) + pow(SICCloudData::nz, 2));
    assert(mag > 0);
    SICCloudData::nx /= mag;
    SICCloudData::ny /= mag;
    SICCloudData::nz /= mag;

    if (isroot)
    {
        cout << "INITIAL SHOCK" << endl;
        cout << '\t' << "p-Ratio         = " << SICCloudData::pressureRatio << endl;
        cout << '\t' << "Mach            = " << SICCloudData::mach << endl;
        cout << '\t' << "Shock speed     = " << SICCloudData::u0 + SICCloudData::c0*SICCloudData::mach<< endl;
        cout << '\t' << "Shock direction = (" << SICCloudData::nx << ", " << SICCloudData::ny << ", " << SICCloudData::nz << ")" << endl;
        cout << '\t' << "Point on shock  = (" << SICCloudData::Sx << ", " << SICCloudData::Sy << ", " << SICCloudData::Sz << ")" << endl << endl;
        cout << "INITIAL MATERIALS" << endl;
        cout << '\t' << "Material 0:" << endl;
        cout << "\t\t" << "gamma = " << MaterialDictionary::gamma1 << endl;
        cout << "\t\t" << "pc    = " << MaterialDictionary::pc1 << endl;
        cout << "\t\t" << "Pre-Shock:" << endl;
        cout << "\t\t\t" << "rho = " << SICCloudData::rho0 << endl;
        cout << "\t\t\t" << "u   = " << SICCloudData::u0 << endl;
        cout << "\t\t\t" << "p   = " << SICCloudData::p0 << endl;
        cout << "\t\t" << "Post-Shock:" << endl;
        cout << "\t\t\t" << "rho = " << SICCloudData::rho1 << endl;
        cout << "\t\t\t" << "u   = " << SICCloudData::u1 << endl;
        cout << "\t\t\t" << "p   = " << SICCloudData::p1 << endl;
        cout << '\t' << "Material 1:" << endl;
        cout << "\t\t" << "gamma = " << MaterialDictionary::gamma2 << endl;
        cout << "\t\t" << "pc    = " << MaterialDictionary::pc2 << endl;
        cout << "\t\t" << "Pre-Shock:" << endl;
        cout << "\t\t\t" << "rho = " << SICCloudData::rhoB << endl;
        cout << "\t\t\t" << "u   = " << SICCloudData::uB << endl;
        cout << "\t\t\t" << "p   = " << SICCloudData::pB << endl << endl;
    }

    // This sets the initial conditions. This method (_ic()) is called at the
    // end of the general _setup() method defined in the parent class
    Seed<shape> *myseed = NULL;
    _set_cloud(&myseed);
    _ic_quad(myseed);
    delete myseed;
}

bool Sim_SICCloudMPI::_restart()
{
    _set_constants();
    return Sim_SteadyStateMPI::_restart();
}

void Sim_SICCloudMPI::_initialize_cloud()
{
    ifstream f_read("cloud_config.dat");

    if (!f_read.good())
    {
        cerr << "Error: Can not find cloud_config.dat. Abort...\n";
        abort();
    }

    f_read >> SICCloudData::n_shapes;
    /* TODO: (Thu 30 Oct 2014 04:17:08 PM CET) Clean this shit at some time! I
     * want to read a file of bubbles here only, the cloud MUST be created with
     * an external client */
    /* f_read >> SICCloudData::n_small; */
    /* f_read >> SICCloudData::min_rad >> SICCloudData::max_rad; */
    /* f_read >> SICCloudData::seed_s[0] >> SICCloudData::seed_s[1] >> SICCloudData::seed_s[2]; */
    /* f_read >> SICCloudData::seed_e[0] >> SICCloudData::seed_e[1] >> SICCloudData::seed_e[2]; */
    /* if (!f_read.eof()) */
    /*     f_read >> SICCloudData::n_sensors; */
    /* else */
    /*     SICCloudData::n_sensors=0; */

    f_read.close();

    if (isroot && verbosity)
        printf("cloud data: N %d Nsmall %d Rmin %f Rmax %f s=%f,%f,%f e=%f,%f,%f\n",
                SICCloudData::n_shapes, SICCloudData::n_small, SICCloudData::min_rad, SICCloudData::max_rad,
                SICCloudData::seed_s[0], SICCloudData::seed_s[1], SICCloudData::seed_s[2],
                SICCloudData::seed_e[0], SICCloudData::seed_e[1], SICCloudData::seed_e[2]);
}

void Sim_SICCloudMPI::_set_cloud(Seed<shape> **seed)
{
    const MPI_Comm cart_world = mygrid->getCartComm();

    /* TODO: (Thu 30 Oct 2014 11:04:35 AM CET) what is really necessary in
     * cloud_config file? n_shapes yes  */
    if(isroot) _initialize_cloud(); // requires file cloud_config.dat

    MPI_Bcast(&SICCloudData::n_shapes,    1, MPI_INT,    0, cart_world);
    MPI_Bcast(&SICCloudData::n_small,     1, MPI_INT,    0, cart_world);
    MPI_Bcast(&SICCloudData::small_count, 1, MPI_INT,    0, cart_world);
    MPI_Bcast(&SICCloudData::min_rad,     1, MPI_DOUBLE, 0, cart_world);
    MPI_Bcast(&SICCloudData::max_rad,     1, MPI_DOUBLE, 0, cart_world);
    MPI_Bcast(SICCloudData::seed_s,       3, MPI_DOUBLE, 0, cart_world);
    MPI_Bcast(SICCloudData::seed_e,       3, MPI_DOUBLE, 0, cart_world);
    MPI_Bcast(&SICCloudData::n_sensors,   1, MPI_INT,    0, cart_world);

    Seed<shape> * const newseed = new Seed<shape>(SICCloudData::seed_s, SICCloudData::seed_e);
    assert(newseed != NULL);

    vector<shape> v(SICCloudData::n_shapes);

    if(isroot)
    {
        newseed->make_shapes(SICCloudData::n_shapes, "cloud.dat", mygrid->getH());
        v = newseed->get_shapes();
    }

    MPI_Bcast(&v.front(), v.size() * sizeof(shape), MPI::CHAR, 0, cart_world);

    if (!isroot) newseed->set_shapes(v);

    assert(newseed->get_shapes_size() > 0 && newseed->get_shapes_size() == SICCloudData::n_shapes);

    double myorigin[3], myextent[3];
    mygrid->get_origin(myorigin);
    mygrid->get_extent(myextent);
    *newseed = newseed->retain_shapes(myorigin, myextent);

    *seed = newseed;

    MPI_Barrier(cart_world);
}

void Sim_SICCloudMPI::_set_sensors()
{
}

static Real is_shock(const Real P[3])
{
    const Real nx = SICCloudData::nx;
    const Real ny = SICCloudData::ny;
    const Real nz = SICCloudData::nz;
    const Real Sx = SICCloudData::Sx;
    const Real Sy = SICCloudData::Sy;
    const Real Sz = SICCloudData::Sz;

    // no need to divide by n*n, since n*n = 1; d is the distance of the
    // projection of point P onto the planar initial shock.
    const Real d = -(nx*(Sx-P[0]) + ny*(Sy-P[1]) + nz*(Sz-P[2]));

    return SimTools::heaviside(d);
}

template <typename T>
static T set_IC(const Real shock, const Real bubble)
{
    T out;

    const Real pre_shock[3]    = {SICCloudData::rho0, SICCloudData::u0, SICCloudData::p0};
    const Real post_shock[3]   = {SICCloudData::rho1, SICCloudData::u1, SICCloudData::p1};
    const Real bubble_state[3] = {SICCloudData::rhoB, SICCloudData::uB, SICCloudData::pB};

    const Real G1 = SICCloudData::g1-1;
    const Real G2 = SICCloudData::g2-1;
    const Real F1 = SICCloudData::g1*SICCloudData::pc1;
    const Real F2 = SICCloudData::g2*SICCloudData::pc2;

    // WARNING: THIS ONLY WORKS IF THE NORMAL VECTOR OF THE PLANAR SHOK IS
    // colinear WITH EITHER UNIT VECTOR OF THE GLOBAL COORDINATE SYSTEM!
    out.rho = shock*post_shock[0] + (1-shock)*(bubble_state[0]*bubble + pre_shock[0]*(1-bubble));
    out.u   = (shock*post_shock[1] + (1-shock)*(bubble_state[1]*bubble + pre_shock[1]*(1-bubble)))*out.rho;
    out.v   = 0.0;
    out.w   = 0.0;

    // component mix (Note: shock = 1 implies bubble = 0)
    out.G = 1./G1*(1-bubble) + 1./G2*bubble;
    out.P = F1/G1*(1-bubble) + F2/G2*bubble;

    // energy
    const Real pressure  = shock*post_shock[2] + (1-shock)*(bubble_state[2]*bubble + pre_shock[2]*(1-bubble));
    const Real ke = 0.5*(out.u*out.u + out.v*out.v + out.w*out.w)/out.rho;
    out.energy = pressure*out.G + out.P + ke;

    return out;
}


template <typename T>
static T get_IC(const Real pos[3], const vector<shape> * const blockShapes)
{
    T IC;

    const Real shock = is_shock(pos);

    /* *
     * For the IC, bubbles are not allowed to be located in the post shock
     * region.  No check is being performed.
     * */
    if (!blockShapes)
    {
        // If blockShapes == NULL, it is assumed there are no shapes in this
        // block.  Therefore, only the shock wave is treated for initial
        // conditions.
        IC = set_IC<T>(shock, 0);
    }
    else
    {
        // There are shapes within this block to be taken care of.
        const Real bubble = eval(*blockShapes, pos);
        IC = set_IC<T>(shock, bubble);
    }

    return IC;
}

template<typename T>
static T integral(const Real p[3], const Real h, const vector<shape> * const blockShapes) // h should be cubism h/2
{
    T samples[3][3][3];
    T zintegrals[3][3];
    T yzintegrals[3];

    const Real x0[3] = {p[0] - h, p[1] - h, p[2] - h};

    for(int iz=0; iz<3; iz++)
        for(int iy=0; iy<3; iy++)
            for(int ix=0; ix<3; ix++)
            {
                const Real mypos[3] = {x0[0]+ix*h, x0[1]+iy*h, x0[2]+iz*h};
                samples[iz][iy][ix] = get_IC<T>(mypos, blockShapes);
            }

    for(int iy=0; iy<3; iy++)
        for(int ix=0; ix<3; ix++)
            zintegrals[iy][ix] = (1/6.) * samples[0][iy][ix]+(2./3) * samples[1][iy][ix]+(1/6.)* samples[2][iy][ix];

    for(int ix=0; ix<3; ix++)
        yzintegrals[ix] = (1./6)*zintegrals[0][ix] + (2./3)*zintegrals[1][ix]+(1./6)*zintegrals[2][ix];

    return (1./6) * yzintegrals[0]+(2./3) * yzintegrals[1]+(1./6)* yzintegrals[2];
}


void Sim_SICCloudMPI::_ic_quad(const Seed<shape> * const seed)
{
    if (seed->get_shapes().size() == 0)
    {
#pragma omp parallel for
        for (int i=0; i < (int)subblocks.size(); ++i)
        {
            MenialBlock& myblock = subblocks[i];

            for (int iz=0; iz < MenialBlock::sizeZ; ++iz)
                for (int iy=0; iy < MenialBlock::sizeY; ++iy)
                    for (int ix=0; ix < MenialBlock::sizeX; ++ix)
                    {
                        Real pos[3];
                        myblock.get_pos(ix, iy, iz, pos);
                        const FluidElement IC = get_IC<FluidElement>(pos, NULL);
                        myblock.set(ix, iy, iz, IC);
                    }
        }
    }
    else
    {
        const Real h  = mygrid->getH();
        const double myextent[3] = {
            MenialBlock::extent_x,
            MenialBlock::extent_y,
            MenialBlock::extent_z };

#pragma omp parallel for
        for (int i=0; i < (int)subblocks.size(); ++i)
        {
            MenialBlock& myblock = subblocks[i];

            double block_origin[3];
            myblock.get_origin(block_origin);

            vector<shape> myshapes = seed->get_shapes().size() ? seed->retain_shapes(block_origin, myextent).get_shapes() : seed->get_shapes();
            const bool isempty = myshapes.size() == 0;

            if (isempty)
            {
                for (int iz=0; iz < MenialBlock::sizeZ; ++iz)
                    for (int iy=0; iy < MenialBlock::sizeY; ++iy)
                        for (int ix=0; ix < MenialBlock::sizeX; ++ix)
                        {
                            Real pos[3];
                            myblock.get_pos(ix, iy, iz, pos);
                            const FluidElement IC = get_IC<FluidElement>(pos, NULL);
                            myblock.set(ix, iy, iz, IC);
                        }
            }
            else
            {
                for (int iz=0; iz < MenialBlock::sizeZ; ++iz)
                    for (int iy=0; iy < MenialBlock::sizeY; ++iy)
                        for (int ix=0; ix < MenialBlock::sizeX; ++ix)
                        {
                            Real pos[3];
                            myblock.get_pos(ix, iy, iz, pos);
                            const FluidElement IC = integral<FluidElement>(pos, 0.5*h, &myshapes);
                            myblock.set(ix, iy, iz, IC);
                        }
            }
        }
    }
}


void Sim_SICCloudMPI::run()
{
    _setup();

    /* TODO: (Thu 30 Oct 2014 02:02:50 PM CET) Analysis is currently only
     * implemented for this case */
    parser.set_strict_mode();
    int analysisperiod = parser("-analysisperiod").asInt();
    parser.unset_strict_mode();

    if (parser("-printargs").asBool(true) && isroot) parser.print_options();

    // log dumps
    FILE* fp;
    if (bIO) fp = fopen("dump.log", "a");

    // dynamic settings file
    ofstream current_setting("parameter.dat");
    current_setting << tend << endl;
    current_setting << CFL << endl;
    current_setting << saveperiod << endl;
    current_setting << analysisperiod << endl;
    current_setting << nsteps << endl;
    current_setting.close();


    if (dryrun)
    {
        if (isroot) printf("Dry Run...\n");
        return;
    }
    else
    {
        double dt, dt_max;

        const uint_t step_start = LSRK3_DataMPI::step; // such that -nsteps is a relative measure (only relevant for restarts)

        /* _dump_statistics(LSRK3_DataMPI::step, LSRK3_DataMPI::time, 0.0); */

        while (LSRK3_DataMPI::time < tend)
        {
            const double dt_final = tend - LSRK3_DataMPI::time;
            const double dt_dump  = tnextdump - LSRK3_DataMPI::time;

            // here is where the stuff happens
            profiler.push_start("EVOLVE");
            dt_max = dt_final;
            if (bIO) dt_max = dt_max < dt_dump ? dt_max : dt_dump;
            dt = (*stepper)(dt_max); // step ahead, sucka
            profiler.pop_stop();

            // post processings
            if (isroot) printf("step id is %d, time %e (dt = %e)\n", LSRK3_DataMPI::step, LSRK3_DataMPI::time, dt);

            if (bIO && (float)LSRK3_DataMPI::time == (float)tnextdump)
            {
                fprintf(fp, "step=%d\ttime=%e\n", LSRK3_DataMPI::step, LSRK3_DataMPI::time);
                fflush(fp);
                tnextdump += dumpinterval;
                if (isroot) printf("I have to take a dump...\n");
                _take_a_dump();
            }

            if (LSRK3_DataMPI::step % saveperiod == 0)
            {
                profiler.push_start("SAVE");
                if (isroot) printf("Saving time step...\n");
                _save();
                profiler.pop_stop();
            }

            if (LSRK3_DataMPI::step % analysisperiod == 0)
            {
                profiler.push_start("ANALYSIS");
                if (isroot) printf("Running analysis...\n");
                _dump_statistics(LSRK3_DataMPI::step, LSRK3_DataMPI::time, dt);
                profiler.pop_stop();
            }

            if (LSRK3_DataMPI::step % 20 == 0)
            {
                if (isroot)
                {
                    ifstream new_settings("parameter.dat");
                    new_settings >> tend;
                    new_settings >> CFL;
                    new_settings >> saveperiod;
                    new_settings >> analysisperiod;
                    new_settings >> nsteps;
                    new_settings.close();
                }

                const MPI_Comm cart_world = mygrid->getCartComm();
                MPI_Bcast(&tend, 1, MPI_DOUBLE, 0, cart_world);
                MPI_Bcast(&CFL,  1, MPI_DOUBLE, 0, cart_world);
                MPI_Bcast(&saveperiod,     1, MPI_INT, 0, cart_world);
                MPI_Bcast(&analysisperiod, 1, MPI_INT, 0, cart_world);
                MPI_Bcast(&nsteps,         1, MPI_INT, 0, cart_world);

                stepper->set_CFL(CFL);

                profiler.printSummary();
            }

            if ((LSRK3_DataMPI::step - step_start) == nsteps) break;
        }

        if (isroot) profiler.printSummary();

        if (bIO)
        {
            // dump final data
            fprintf(fp, "step=%d\ttime=%e\n", LSRK3_DataMPI::step, LSRK3_DataMPI::time);
            _take_a_dump();
            fclose(fp);
        }
    }
    return;
}
