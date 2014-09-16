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
using namespace std;

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


Sim_SICCloudMPI::Sim_SICCloudMPI(const int argc, const char ** argv, const int isroot) :
    Sim_SteadyStateMPI(argc, argv, isroot)
{ }

void Sim_SICCloudMPI::_allocGPU()
{
    if (isroot) printf("Allocating GPUlabSICCloud...\n");
    myGPU = new GPUlabSICCloud(*mygrid, nslices, verbosity);
}

void Sim_SICCloudMPI::_dump(const string basename)
{
    const string dump_path = parser("-fpath").asString(".");

    if (isroot) printf("Dumping data%04d at step %d, time %f\n", fcount, step, t);
    sprintf(fname, "%s_%04d-p", basename.c_str(), fcount);
    DumpHDF5_MPI<GridMPI, myPressureStreamer>(*mygrid, step, fname, dump_path);
    sprintf(fname, "%s_%04d-G", basename.c_str(), fcount);
    DumpHDF5_MPI<GridMPI, myGammaStreamer>(*mygrid, step, fname, dump_path);
    /* sprintf(fname, "%s_%04d-P", basename.c_str(), fcount); */
    /* DumpHDF5_MPI<GridMPI, myPiStreamer>(*mygrid, step, fname, dump_path); */
    /* sprintf(fname, "%s_%04d-rho", basename.c_str(), fcount); */
    /* DumpHDF5_MPI<GridMPI, myRhoStreamer>(*mygrid, step, fname, dump_path); */
    /* sprintf(fname, "%s_%04d-velocity", basename.c_str(), fcount); */
    /* DumpHDF5_MPI<GridMPI, myVelocityStreamer>(*mygrid, step, fname, dump_path); */
    /* sprintf(fname, "%s_%04d-energy", basename.c_str(), fcount); */
    /* DumpHDF5_MPI<GridMPI, myEnergyStreamer>(*mygrid, step, fname, dump_path); */
    ++fcount;
}

void Sim_SICCloudMPI::_dump_statistics(const int step_id, const Real t, const Real dt)
{
    double rInt=0., uInt=0., vInt=0., wInt=0., eInt=0., vol=0., ke=0., r2Int=0., mach_max=-HUGE_VAL, p_max=-HUGE_VAL;
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

        mach_max = max(mach_max, IvI/c);
        p_max    = max(p_max, pressure);
    }

    FILE * f = fopen("integrals.dat", "a");
    fprintf(f, "%d %e %e %e %e %e %e %e %e %e %e %e %e %e\n", step_id, t, dt, rInt*h3, uInt*h3,
            vInt*h3, wInt*h3, eInt*h3, vol*h3, ke*h3, r2Int*h3, mach_max, p_max, pow(0.75*vol*h3/M_PI,1./3.));
    fclose(f);
}

void Sim_SICCloudMPI::_dump_sensors(const int step_id, const Real t, const Real dt)
{
}

void Sim_SICCloudMPI::_ic()
{
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

    // shock orienation
    SICCloudData::nx = parser("-shockNx").asDouble(1.0);
    SICCloudData::ny = parser("-shockNy").asDouble(0.0);
    SICCloudData::nz = parser("-shockNz").asDouble(0.0);
    // point on shock
    SICCloudData::Sx = parser("-shockSx").asDouble(0.05);
    SICCloudData::Sy = parser("-shockSy").asDouble(0.0);
    SICCloudData::Sz = parser("-shockSz").asDouble(0.0);

    SICCloudData::g1  = parser("-g1").asDouble(6.12);
    SICCloudData::g2  = parser("-g2").asDouble(1.4);
    SICCloudData::pc1 = parser("-pc1").asDouble(3.43e8/p_scale);
    SICCloudData::pc2 = parser("-pc2").asDouble(0.0);
    MaterialDictionary::gamma1 = SICCloudData::g1;
    MaterialDictionary::gamma2 = SICCloudData::g2;
    MaterialDictionary::pc1 = SICCloudData::pc1;
    MaterialDictionary::pc2 = SICCloudData::pc2;

    // normalize shock normal vector
    const Real mag = sqrt(pow(SICCloudData::nx, 2) + pow(SICCloudData::ny, 2) + pow(SICCloudData::nz, 2));
    assert(mag > 0);
    SICCloudData::nx /= mag;
    SICCloudData::ny /= mag;
    SICCloudData::nz /= mag;

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

    // REALITY CHECK: this only works for normal shock in x-direction
    SICCloudData::post_shock_conservative[0] = SICCloudData::rho1;
    SICCloudData::post_shock_conservative[1] = SICCloudData::rho1*SICCloudData::u1;
    SICCloudData::post_shock_conservative[2] = static_cast<Real>(0.0);
    SICCloudData::post_shock_conservative[3] = static_cast<Real>(0.0);
    const Real G1 = 1.0 / (gamma - 1.0);
    SICCloudData::post_shock_conservative[4] = p1*G1 + pc*gamma*G1 + static_cast<Real>(0.5)*SICCloudData::rho1*SICCloudData::u1*SICCloudData::u1; // v = w = 0 !
    SICCloudData::post_shock_conservative[5] = G1;
    SICCloudData::post_shock_conservative[6] = pc*gamma*G1;

    if (isroot)
    {
        cout << "INITIAL SHOCK" << endl;
        cout << '\t' << "p-Ratio         = " << SICCloudData::pressureRatio << endl;
        cout << '\t' << "Mach            = " << SICCloudData::mach << endl;
        cout << '\t' << "Shock speed     = " << u0 + SICCloudData::c0*SICCloudData::mach<< endl;
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
    // end of _setup().
    Seed<shape> *myseed = NULL;
    _set_cloud(&myseed);
    _ic_quad(myseed);
    delete myseed;
}

void Sim_SICCloudMPI::_initialize_cloud()
{
    ifstream f_read("cloud_config.dat");

    if (!f_read.good())
    {
        cout << "Error: Can not find cloud_config.dat. Abort...\n";
        abort();
    }

    f_read >> SICCloudData::n_shapes >> SICCloudData::n_small;
    f_read >> SICCloudData::min_rad >> SICCloudData::max_rad;
    f_read >> SICCloudData::seed_s[0] >> SICCloudData::seed_s[1] >> SICCloudData::seed_s[2];
    f_read >> SICCloudData::seed_e[0] >> SICCloudData::seed_e[1] >> SICCloudData::seed_e[2];
    if (!f_read.eof())
        f_read >> SICCloudData::n_sensors;
    else
        SICCloudData::n_sensors=0;

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

    if(isroot) _initialize_cloud(); // requires file cloud_config.dat

    MPI_Bcast(&SICCloudData::n_shapes,    1, MPI::INT,    0, cart_world);
    MPI_Bcast(&SICCloudData::n_small,     1, MPI::INT,    0, cart_world);
    MPI_Bcast(&SICCloudData::small_count, 1, MPI::INT,    0, cart_world);
    MPI_Bcast(&SICCloudData::min_rad,     1, MPI::DOUBLE, 0, cart_world);
    MPI_Bcast(&SICCloudData::max_rad,     1, MPI::DOUBLE, 0, cart_world);
    MPI_Bcast(SICCloudData::seed_s,       3, MPI::DOUBLE, 0, cart_world);
    MPI_Bcast(SICCloudData::seed_e,       3, MPI::DOUBLE, 0, cart_world);
    MPI_Bcast(&SICCloudData::n_sensors,   1, MPI::INT,    0, cart_world);

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

    // no need to divide by n*n, since n*n = 1
    const Real d = -(nx*(Sx-P[0]) + ny*(Sy-P[1]) + nz*(Sz-P[2]));

    return SimTools::heaviside(d);
}

struct FluidElement
{
    Real rho, u, v, w, energy, G, P;
    FluidElement() : rho(-1), u(0), v(0), w(0), energy(-1), G(-1), P(-1) { }
    FluidElement(const FluidElement& e): rho(e.rho), u(e.u), v(e.v), w(e.w), energy(e.energy), G(e.G), P(e.P) { }
    FluidElement& operator=(const FluidElement& e)
    {
        if (this != &e) {rho = e.rho; u = e.u; v = e.v; w = e.w; energy = e.energy; G = e.G; P = e.P;}
        return *this;
    }
    FluidElement operator+(const FluidElement& e) const
    {
        FluidElement sum(e);
        sum.rho += rho; sum.u += u; sum.v += v; sum.w += w; sum.energy += energy; sum.G += G; sum.P += P;
        return sum;
    }
    friend FluidElement operator*(const Real s, const FluidElement& e);
};

FluidElement operator*(const Real s, const FluidElement& e)
{
    FluidElement prod(e);
    prod.rho *= s; prod.u *= s; prod.v *= s; prod.w *= s; prod.energy *= s; prod.G *= s; prod.P *= s;
    return prod;
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


class SubGrid
{
    public:
    class SubBlock
    {
        public:
        static uint_t sizeX;
        static uint_t sizeY;
        static uint_t sizeZ;
        static double extent_x;
        static double extent_y;
        static double extent_z;
        static double h;

        private:
        const double origin[3];
        const uint_t block_index[3];
        GridMPI& grid;

        public:
        SubBlock(const double O[3], const uint_t idx[3], GridMPI& G) : origin{O[0], O[1], O[2]}, block_index{idx[0], idx[1], idx[2]}, grid(G) { }

        inline void get_pos(const unsigned int ix, const unsigned int iy, const unsigned int iz, Real pos[3]) const
        {
            // local position, relative to origin, cell center
            pos[0] = origin[0] + h * (ix+0.5);
            pos[1] = origin[1] + h * (iy+0.5);
            pos[2] = origin[2] + h * (iz+0.5);
        }
        inline void get_index(const unsigned int lix, const unsigned int liy, const unsigned int liz, uint_t gidx[3]) const
        {
            // returns global grid index
            gidx[0] = block_index[0] * sizeX + lix;
            gidx[1] = block_index[1] * sizeY + liy;
            gidx[2] = block_index[2] * sizeZ + liz;
        }
        inline void get_origin(double O[3]) const
        {
            O[0] = origin[0];
            O[1] = origin[1];
            O[2] = origin[2];
        }

        void set(const int lix, const int liy, const int liz, const FluidElement& IC)
        {
            const int ix = block_index[0] * sizeX + lix;
            const int iy = block_index[1] * sizeY + liy;
            const int iz = block_index[2] * sizeZ + liz;

            grid(ix, iy, iz, GridMPI::PRIM::R) = IC.rho;
            grid(ix, iy, iz, GridMPI::PRIM::U) = IC.u;
            grid(ix, iy, iz, GridMPI::PRIM::V) = IC.v;
            grid(ix, iy, iz, GridMPI::PRIM::W) = IC.w;
            grid(ix, iy, iz, GridMPI::PRIM::E) = IC.energy;
            grid(ix, iy, iz, GridMPI::PRIM::G) = IC.G;
            grid(ix, iy, iz, GridMPI::PRIM::P) = IC.P;
        }
    };


    private:
    const uint_t nblock_x, nblock_y, nblock_z;
    vector<SubBlock *> blocks;


    public:
    SubGrid(GridMPI *grid, const uint_t ncX, const uint_t ncY, const uint_t ncZ) :
        nblock_x(_BLOCKSIZEX_/ncX), nblock_y(_BLOCKSIZEY_/ncY), nblock_z(_BLOCKSIZEZ_/ncZ)
    {
        if (_BLOCKSIZEX_ % ncX != 0)
        {
            fprintf(stderr, "ERROR: subcellsX must be an integer multiple of _BLOCKSIZEX_");
            abort();
        }
        if (_BLOCKSIZEY_ % ncY != 0)
        {
            fprintf(stderr, "ERROR: subcellsY must be an integer multiple of _BLOCKSIZEY_");
            abort();
        }
        if (_BLOCKSIZEZ_ % ncZ != 0)
        {
            fprintf(stderr, "ERROR: subcellsZ must be an integer multiple of _BLOCKSIZEZ_");
            abort();
        }

        const double h_ = grid->getH();
        SubGrid::SubBlock::sizeX = ncX;
        SubGrid::SubBlock::sizeY = ncY;
        SubGrid::SubBlock::sizeZ = ncZ;
        SubGrid::SubBlock::extent_x = ncX * h_;
        SubGrid::SubBlock::extent_y = ncY * h_;
        SubGrid::SubBlock::extent_z = ncZ * h_;
        SubGrid::SubBlock::h = h_;

        blocks.reserve(nblock_x * nblock_y * nblock_z);

        double O[3];
        grid->get_origin(O);
        for (int biz=0; biz < nblock_z; ++biz)
            for (int biy=0; biy < nblock_y; ++biy)
                for (int bix=0; bix < nblock_x; ++bix)
                {
                    const double thisOrigin[3] = {
                        O[0] + bix * SubGrid::SubBlock::extent_x,
                        O[1] + biy * SubGrid::SubBlock::extent_y,
                        O[2] + biz * SubGrid::SubBlock::extent_z };
                    const uint_t thisIndex[3] = {bix, biy, biz};

                    SubBlock *thisBlock = new SubBlock(thisOrigin, thisIndex, *grid);
                    blocks.push_back(thisBlock);
                }
    }

    ~SubGrid()
    {
        for (int i =0; i < (int)blocks.size(); ++i)
            delete blocks[i];
        blocks.clear();
    }

    SubBlock *operator[](const int block_id) { return blocks[block_id]; }
    const size_t size() const { return blocks.size(); }
};

uint_t SubGrid::SubBlock::sizeX = 0;
uint_t SubGrid::SubBlock::sizeY = 0;
uint_t SubGrid::SubBlock::sizeZ = 0;
double SubGrid::SubBlock::extent_x = 0.0;
double SubGrid::SubBlock::extent_y = 0.0;
double SubGrid::SubBlock::extent_z = 0.0;
double SubGrid::SubBlock::h = 0.0;


void Sim_SICCloudMPI::_ic_quad(const Seed<shape> * const seed)
{
    // we use artifical subblocks that are smaller than the original block
    parser.set_strict_mode();
    const int subcells_x = parser("-subcellsX").asInt();
    parser.unset_strict_mode();
    const int subcells_y = parser("-subcellsY").asInt(subcells_x);
    const int subcells_z = parser("-subcellsZ").asInt(subcells_x);

    SubGrid subblocks(mygrid, subcells_x, subcells_y, subcells_z);

    if (seed->get_shapes().size() == 0)
    {
#pragma omp parallel for
        for (int i=0; i < (int)subblocks.size(); ++i)
        {
            SubGrid::SubBlock& myblock = *(subblocks[i]);

            for (int iz=0; iz < SubGrid::SubBlock::sizeZ; ++iz)
                for (int iy=0; iy < SubGrid::SubBlock::sizeY; ++iy)
                    for (int ix=0; ix < SubGrid::SubBlock::sizeX; ++ix)
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
            SubGrid::SubBlock::extent_x,
            SubGrid::SubBlock::extent_y,
            SubGrid::SubBlock::extent_z };

#pragma omp parallel for
        for (int i=0; i < (int)subblocks.size(); ++i)
        {
            SubGrid::SubBlock& myblock = *(subblocks[i]);

            double block_origin[3];
            myblock.get_origin(block_origin);

            vector<shape> myshapes = seed->get_shapes().size() ? seed->retain_shapes(block_origin, myextent).get_shapes() : seed->get_shapes();
            const bool isempty = myshapes.size() == 0;

            if (isempty)
            {
                for (int iz=0; iz < SubGrid::SubBlock::sizeZ; ++iz)
                    for (int iy=0; iy < SubGrid::SubBlock::sizeY; ++iy)
                        for (int ix=0; ix < SubGrid::SubBlock::sizeX; ++ix)
                        {
                            Real pos[3];
                            myblock.get_pos(ix, iy, iz, pos);
                            const FluidElement IC = get_IC<FluidElement>(pos, NULL);
                            myblock.set(ix, iy, iz, IC);
                        }
            }
            else
            {
                for (int iz=0; iz < SubGrid::SubBlock::sizeZ; ++iz)
                    for (int iy=0; iy < SubGrid::SubBlock::sizeY; ++iy)
                        for (int ix=0; ix < SubGrid::SubBlock::sizeX; ++ix)
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

    parser.set_strict_mode();
    const uint_t analysisperiod = parser("-analysisperiod").asInt();
    parser.unset_strict_mode();

    if (dryrun)
    {
        if (isroot) printf("Dry Run...\n");
        return;
    }
    else
    {
        double dt, dt_max;

        const uint_t step_start = step; // such that -nsteps is a relative measure (only relevant for restarts)
        while (t < tend)
        {
            profiler.push_start("EVOLVE");
            dt_max = (tend-t) < (tnextdump-t) ? (tend-t) : (tnextdump-t);
            dt = (*stepper)(dt_max);
            profiler.pop_stop();

            t += dt;
            ++step;

            if (isroot) printf("step id is %d, physical time %e (dt = %e)\n", step, t, dt);

            if ((float)t == (float)tnextdump)
            {
                profiler.push_start("DUMP");
                tnextdump += dumpinterval;
                _dump();
                profiler.pop_stop();
            }

            if (step % saveperiod == 0)
            {
                profiler.push_start("SAVE");
                if (isroot) printf("Saving time step...\n");
                _save();
                profiler.pop_stop();
            }

            if (step % analysisperiod == 0)
            {
                profiler.push_start("ANALYSIS");
                if (isroot) printf("Running analysis...\n");
                _dump_statistics(step, t, dt);
                profiler.pop_stop();
            }

            if (step % 10 == 0)
                profiler.printSummary();

            if ((step-step_start) == nsteps) break;
        }

        profiler.printSummary();

        _dump();

        return;
    }
}
