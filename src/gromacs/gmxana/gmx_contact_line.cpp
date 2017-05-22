/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2004, The GROMACS development team.
 * Copyright (c) 2013,2014,2015, by the GROMACS development team, led by
 * Mark Abraham, David van der Spoel, Berk Hess, and Erik Lindahl,
 * and including many others, as listed in the AUTHORS file in the
 * top-level source directory and at http://www.gromacs.org.
 *
 * GROMACS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * GROMACS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GROMACS; if not, see
 * http://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at http://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out http://www.gromacs.org.
 */
#include "gmxpre.h"

#include <algorithm> // find
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iterator> // distance
#include <set> // distance
#include <vector>

#include "gromacs/commandline/pargs.h"
#include "gromacs/commandline/viewit.h"
#include "gromacs/fileio/confio.h"
#include "gromacs/fileio/trxio.h"
#include "gromacs/fileio/xvgr.h"
#include "gromacs/gmxana/gmx_ana.h"
#include "gromacs/math/units.h"
#include "gromacs/math/vec.h"
#include "gromacs/pbcutil/pbc.h"
#include "gromacs/pbcutil/rmpbc.h"
#include "gromacs/topology/index.h"
#include "gromacs/topology/topology.h"
#include "gromacs/utility/arraysize.h"
#include "gromacs/utility/exceptions.h"
#include "gromacs/utility/fatalerror.h"
#include "gromacs/utility/futil.h"
#include "gromacs/utility/gmxomp.h"
#include "gromacs/utility/smalloc.h"

using namespace std;

//#define DEBUG_CONTACTLINE

/****************************************************************************
 * This program analyzes how contact line molecules advance.                *
 * Petter Johansson, Stockholm 2017                                         *
 ****************************************************************************/

enum class Algorithm {
    ContactLine = 1,
    Bottom = 2,
    Hops = 3
};

struct CLConf {
    CLConf(t_pargs    pa[],
           const int  pasize,
           const rvec rmin_in,
           const rvec rmax_in,
           const int  alg_in)
        :algorithm{ static_cast<Algorithm>(alg_in) },
         cutoff{ opt2parg_real("-co", pasize, pa) },
         cutoff2{ cutoff * cutoff },
         precision{ opt2parg_real("-prec", pasize, pa) },
         dx{ opt2parg_real("-dx", pasize, pa) },
         hop_min{ opt2parg_real("-hmin", pasize, pa) },
         hop_min2{ hop_min * hop_min },
         hop_max{ opt2parg_real("-hmax", pasize, pa) },
         hop_max2{ hop_max * hop_max },
         nmin{ opt2parg_int("-nmin", pasize, pa) },
         nmax{ opt2parg_int("-nmax", pasize, pa) }
    {
        copy_rvec(rmin_in, rmin);
        copy_rvec(rmax_in, rmax);
        set_search_space_limits();

        const auto stride_buf = opt2parg_int("-stride", pasize, pa);
        if (stride_buf < 1)
        {
            gmx_fatal(FARGS, "Input stride must be positive.");
        }
        stride = static_cast<size_t>(stride_buf);
    }

    void set_box_limits(const matrix box)
    {
        rmin[XX] = (rmin[XX] <= 0.0) ? 0.0 : rmin[XX];
        rmin[YY] = (rmin[YY] <= 0.0) ? 0.0 : rmin[YY];
        rmin[ZZ] = (rmin[ZZ] <= 0.0) ? 0.0 : rmin[ZZ];
        rmax[XX] = (rmax[XX] <= 0.0) ? box[XX][XX] : rmax[XX];
        rmax[YY] = (rmax[YY] <= 0.0) ? box[YY][YY] : rmax[YY];
        rmax[ZZ] = (rmax[ZZ] <= 0.0) ? box[ZZ][ZZ] : rmax[ZZ];
        set_search_space_limits();
    }

    void set_search_space_limits()
    {
        const rvec add_ss = {cutoff, cutoff, cutoff};
        rvec_sub(rmin, add_ss, rmin_ss);
        rvec_add(rmax, add_ss, rmax_ss);
    }

    Algorithm algorithm;
    real cutoff,
         cutoff2,
         precision,
         dx,
         hop_min,
         hop_min2,
         hop_max,
         hop_max2;
    int nmin,
        nmax;
    size_t stride;
    rvec rmin,
         rmax,
         rmin_ss,
         rmax_ss;
};

template<typename T>
struct Counter {
    Counter ()
        :duration{static_cast<T>(0.0)} {}

    T total() const { return this->duration.count(); }

    void set() { start = chrono::system_clock::now(); }
    void stop() { duration += chrono::system_clock::now() - start; }

    chrono::system_clock::time_point start;
    chrono::duration<T> duration;
};

struct Timings {
    Counter<double> interface,
                    bottom,
                    contact_line,
                    loop,
                    traj_total;
};

static void
print_a_timing(const char    *label,
               const Counter<double>  counter,
               const Counter<double>  total)
{
    constexpr size_t MAXLEN = 6;
    const auto percent = 100 * (counter.total() / total.total());
    auto percent_str = new char[MAXLEN];
    snprintf(percent_str, MAXLEN, "%3.1f%%", percent);
    fprintf(stderr, "%-27s%12.3f%11s\n", label, counter.total(), percent_str);
}

static void
print_timings(Timings &timings)
{
    const auto subloop_total = timings.interface.duration
        + timings.bottom.duration + timings.contact_line.duration;
    timings.loop.duration -= subloop_total;
    const auto total = timings.traj_total;

    fprintf(stderr, "\n");
    fprintf(stderr, "Counter                        Time (s)   Of total\n");
    fprintf(stderr, "--------------------------------------------------\n");
    print_a_timing("Collect interface", timings.interface, total);
    print_a_timing("Collect bottom", timings.bottom, total);
    print_a_timing("Collect contact line", timings.contact_line, total);
    print_a_timing("Remainder loop", timings.loop, total);
    fprintf(stderr, "--------------------------------------------------\n");
    fprintf(stderr, "In total the trajectory analysis took %.3f seconds.\n",
            total.total());
}

static bool
is_inside_limits(const rvec x,
                 const rvec rmin,
                 const rvec rmax)
{
    return x[XX] >= rmin[XX] && x[XX] < rmax[XX]
        && x[YY] >= rmin[YY] && x[YY] < rmax[YY]
        && x[ZZ] >= rmin[ZZ] && x[ZZ] < rmax[ZZ];
}

static vector<int>
find_interface_indices(const rvec          *x0,
                       const int           *grpindex,
                       const int            grpsize,
                       const struct CLConf &conf,
                       const t_pbc         *pbc)
{
    // Find indices within search volume
    vector<size_t> search_space;
    vector<size_t> candidates;

    for (int i = 0; i < grpsize; ++i)
    {
        const auto n = grpindex[i];
        const auto x1 = x0[n];

        if (is_inside_limits(x1, conf.rmin_ss, conf.rmax_ss))
        {
            search_space.push_back(n);

            if (is_inside_limits(x1, conf.rmin, conf.rmax))
            {
                candidates.push_back(n);
            }
        }
    }

#ifdef DEBUG_CONTACTLINE
    fprintf(stderr, "Kept %lu (%lu) indices within the search volume. ",
            candidates.size(), search_space.size());
#endif

    vector<int> interface_inds;
    interface_inds.reserve(candidates.size());

    for (auto i : candidates)
    {
        const auto x1 = x0[i];
        int count = 0;

        #pragma omp parallel for reduction(+:count)
        for (size_t j = 0; j < search_space.size(); ++j)
        {
            const auto k = search_space[j];
            rvec dx;

            if (i != k)
            {
                const auto x2 = x0[k];
                pbc_dx(pbc, x1, x2, dx);

                if (norm2(dx) <= conf.cutoff2)
                {
                    ++count;
                }
            }
        }

        if (count >= conf.nmin && count <= conf.nmax)
        {
            interface_inds.push_back(i);
        }
    }

    interface_inds.shrink_to_fit();

#ifdef DEBUG_CONTACTLINE
    fprintf(stderr, "Found %lu interface ", interface_inds.size());
#endif

    return interface_inds;
}

static vector<vector<int>>
slice_system_along_dir(const rvec *x0,
                       const vector<int> interface,
                       const CLConf &conf,
                       const int dir)
{
    // Identify the floor by slicing the system and finding the lowest peak
    const int num_slices = static_cast<int>(ceil((conf.rmax[dir] - conf.rmin[dir]) / conf.precision));
    const real final_slice_precision = (conf.rmax[dir] - conf.rmin[dir]) / num_slices;

    vector<vector<int>> slice_indices (num_slices);
    for (auto i : interface)
    {
        const auto x = x0[i][dir];
        const auto slice = static_cast<size_t>(
            (x - conf.rmin[dir]) / final_slice_precision
        );
        slice_indices.at(slice).push_back(i);
    }

    return slice_indices;
}

static vector<int>
find_bottom_layer_indices(const rvec *x0,
                          const vector<int> interface,
                          const CLConf &conf)
{
    const auto slice_indices = slice_system_along_dir(x0, interface, conf, ZZ);

    // Track the maximum count, when a decrease is found the peak
    // was in the previous slice
    int prev_slice = -1;
    size_t max_count = 0;

    for (auto indices : slice_indices)
    {
        const auto count = indices.size();
        if (count < max_count)
        {
            break;
        }

        max_count = count;
        ++prev_slice;
    }

    return slice_indices.at(prev_slice);
}

using VecArray = array<real, DIM>;

struct Interface {
    Interface(const rvec        *x0,
              const int          num_atoms,
              const vector<int>  bottom_indices,
              const vector<int>  int_indices)
        :bottom{bottom_indices},
         interface{int_indices}
    {
        try {
            positions.reserve(num_atoms);

            for (int i = 0; i < num_atoms; ++i)
            {
                positions.push_back({x0[i][XX], x0[i][YY], x0[i][ZZ]});
            }
        }
        catch (const bad_alloc& e) {
            gmx_fatal(FARGS, "Could not allocate memory for all frames (%s).",
                      e.what());
        }
    }

    vector<int> bottom,
                interface;
    set<int> contact_line;
    vector<VecArray> positions;

    // Vectors for absolute contact line positions from rolling
    // a ball over it and the corresponding closest molecular indices
    // for every position.
    vector<real> rolled_xs;
    vector<int>  rolled_inds;
};

static void
add_contact_line(Interface         &interface,
                 const rvec        *x0,
                 const vector<int>  bottom,
                 const CLConf      &conf)
{
    // Roll a ball along the bottom of the interface to detect the contact line
    constexpr real r = 0.28;
    constexpr real r2 = r * r;
    constexpr real dy_ball = r / 5.0;
    constexpr real dx_ball = r / 10.0;

    real y = conf.rmin[YY];

    while (y <= conf.rmax[YY])
    {
        real x = conf.rmax[XX];

        while (x >= conf.rmin[XX])
        {
            vector<int> candidates;
            vector<real> dr2s;

            for (auto index : bottom)
            {
                const auto dx = x - x0[index][XX];
                const auto dy = y - x0[index][YY];
                const auto dr2 = dx * dx + dy * dy;

                if (dr2 <= r2)
                {
                    candidates.push_back(index);
                    dr2s.push_back(dr2);
                }
            }

            if (candidates.size() > 0)
            {
                const auto it = min_element(dr2s.cbegin(), dr2s.cend());
                const auto dn = distance(dr2s.cbegin(), it);
                const auto index = candidates[dn];

                interface.rolled_xs.push_back(x);
                interface.rolled_inds.push_back(index);
                interface.contact_line.insert(index);

                break;
            }

            x -= dx_ball;
        }

        y += dy_ball;
    }

#ifdef DEBUG_CONTACTLINE
    fprintf(stderr, "and %lu contact line atoms.\n",
            interface.contact_line.size());
#endif
}

template<typename T>
static set<int>
find_shared_indices(const set<int> &current,
                    const T &prev)
{
    set<int> shared_indices;

    for (auto i : current)
    {
        if (find(prev.cbegin(), prev.cend(), i) != prev.cend())
        {
            shared_indices.insert(i);
        }
    }

    return shared_indices;
}

static set<int>
contact_line_advancements(const Interface &current,
                          const Interface &previous,
                          const CLConf    &conf)
{
    set<int> inds_advanced;

    /*
    auto cur_index = current.contact_line.cbegin();
    auto prev_index = previous.contact_line.cbegin();

    while ((cur_index != current.contact_line.cend())
            && (prev_index != previous.contact_line.cend()))
    {
        const auto xcur = current.positions[*cur_index][XX];
        const auto xprev = previous.positions[*prev_index][XX];

        if ((xcur - xprev) >= conf.dx)
        {
            inds_advanced.insert(*cur_index);
        }

        ++cur_index;
        ++prev_index;
    }

    set<int> set_advanced;
    */

    if (current.rolled_xs.size() != previous.rolled_xs.size())
    {
        gmx_fatal(FARGS,
                  "Current and previous interface are of different size. "
                  "This should not happen.");
    }

    // The vectors contain X values and have identical order and spacing
    // of Y values, thus we can just iterate through them and compare.
    auto xcur = current.rolled_xs.cbegin();
    auto xprev = previous.rolled_xs.cbegin();
    auto icur = current.rolled_inds.cbegin();

    while (xcur != current.rolled_xs.cend())
    {
        auto dx = *xcur - *xprev;

        if (dx >= conf.dx)
        {
            // Assert that this delta is not due to an atom sidestepping
            // oh-so-slightly
            const auto x1 = current.positions[*icur];
            const auto x2 = previous.positions[*icur];
            const auto dr2 = distance2(x1.data(), x2.data());

            if (dr2 >= conf.hop_min2)
            {
                inds_advanced.insert(*icur);
            }
#ifdef DEBUG_CONTACTLINE
            else
            {
                fprintf(stderr, "Discarded a hop (dr: %.2f)\n", sqrt(dr2));
            }
#endif
        }

        ++xcur;
        ++xprev;
        ++icur;
    }

#ifdef DEBUG_CONTACTLINE
    fprintf(stderr, "Atoms which advanced the contact line:");
    for (auto i : inds_advanced)
    {
        fprintf(stderr, " %d", i);
    }
    fprintf(stderr, "\n");
#endif

    return inds_advanced;
}

static set<int>
at_previous_contact_line(const set<int>  &indices,
                         const Interface &current,
                         const Interface &previous,
                         const CLConf    &conf)
{
    set<int> prev_contact_line;

    switch (conf.algorithm)
    {
        case Algorithm::ContactLine:
            prev_contact_line = find_shared_indices(indices, previous.contact_line);
            break;

        case Algorithm::Bottom:
            prev_contact_line = find_shared_indices(indices, previous.bottom);
            break;

        case Algorithm::Hops:
            {
                const auto shared = find_shared_indices(indices, previous.bottom);

                for (auto index : shared)
                {
                    const auto& x1 = current.positions[index];
                    const auto& x2 = previous.positions[index];

                    const auto dr2 = distance2(x1.data(), x2.data());

                    if ((dr2 >= conf.hop_min2) && (dr2 <= conf.hop_max2))
                    {
                        prev_contact_line.insert(index);
                    }
                }
            }
            break;

        default:
            gmx_fatal(FARGS, "Selected algorithm is not implemented.");
            break;
    }

#ifdef DEBUG_CONTACTLINE
    fprintf(stderr, "Of which were at the previous contact line:");
    for (auto i : prev_contact_line)
    {
        fprintf(stderr, " %d", i);
    }
    fprintf(stderr, "\n");
#endif

    return prev_contact_line;
}

static vector<real>
calculate_fractions(const vector<size_t> &from_previous,
                    const vector<size_t> &num_advanced)
{
    auto from = from_previous.cbegin();
    auto total = num_advanced.cbegin();

    real sum_total = 0.0,
         sum_previous = 0.0,
         sum_fractions = 0.0;

    vector<real> fractions;

    while (from != from_previous.cend())
    {
        const auto tot = static_cast<real>(*total++);
        const auto prev = static_cast<real>(*from++);

        sum_total += tot;
        sum_previous += prev;

        if (tot > 0.0)
        {
            fractions.push_back(prev / tot);
            sum_fractions += fractions.back();
        }
        else
        {
            fractions.push_back(0.0);
        }
    }

    const auto fraction = sum_previous / sum_total;
    const real mean = sum_fractions / static_cast<real>(fractions.size());
    real var = 0.0;

    for (auto f : fractions)
    {
        var += (f - mean) * (f - mean);
    }
    var /= static_cast<real>(fractions.size());
    const auto std = sqrt(var);
    const auto err = std / sqrt(fractions.size());

    fprintf(stdout,
            "Fraction of contact line molecules that came "
            "from the previous contact line:\n%.3f +/- %.3f\n",
            fraction, err);

    return fractions;
}

void
analyze_replacement(const set<int>    &advanced_indices,
                    const Interface   &current,
                    const Interface   &previous,
                    const CLConf      &conf)
{
    for (auto index : advanced_indices)
    {
        // This is the previous position of an atom which
        // advanced the contact line. Find which atom in
        // the *current* bottom layer that is closest to
        // this position. Then find *that* atom's previous
        // position.
        const auto x0 = previous.positions[index];

        // If there are no atoms in the current bottom layer
        // we cannot do anything.!
        auto other = current.bottom.cbegin();
        if (other == current.bottom.cend())
        {
            gmx_fatal(FARGS,
                      "Inconsistent behaviour: No bottom layer of atoms is "
                      "present, but some contact line atoms were found. "
                      "This should not happen!");
        }

        auto x1 = current.positions[*other];
        auto imin = *other;
        auto rmin2 = distance2(x1.data(), x0.data());

        while (other != current.bottom.cend())
        {
            if (*other != index)
            {
                x1 = current.positions[*other];
                const auto dr2 = distance2(x1.data(), x0.data());

                if (dr2 < rmin2)
                {
                    imin = *other;
                    rmin2 = dr2;
                }
            }

            ++other;
        }
    }
}

struct CLData {
    vector<real> times,
                 fractions;
};

static CLData
collect_contact_line_advancement(const char             *fn,
                                 int                    *grpindex,
                                 int                     grpsize,
                                 struct CLConf          &conf,
                                 const t_topology       *top,
                                 const int               ePBC,
                                 const gmx_output_env_t *oenv)
{
    int num_atoms;
    rvec *x0;
    matrix box;
    t_trxstatus *status;
    real t;

    if ((num_atoms = read_first_x(oenv, &status, fn, &t, &x0, box)) == 0)
    {
        gmx_fatal(FARGS, "Could not read coordinates from statusfile\n");
    }


    auto gpbc = gmx_rmpbc_init(&top->idef, ePBC, top->atoms.nr);
    auto pbc = new t_pbc;
    set_pbc(pbc, ePBC, box);
    conf.set_box_limits(box);

    // At each frame, count and save the number of atoms (ie. indices)
    // who advanced the contact line and the number of those who
    // came from the previous contact line. Keep them in a vector
    // to analyze the entire set at the end of the trajectory analysis.
    vector<size_t> num_advanced;
    vector<size_t> num_from_previous;
    vector<real> times;

    // To compare against previous frames when calculating which
    // indices advanced the contact line we need to save this
    // information. A deque gives us a window of the last-to-first data.
    deque<Interface> interfaces;

#ifdef DEBUG_CONTACTLINE
    constexpr size_t MAXLEN = 80;
    auto debug_filename = new char[MAXLEN];
    auto debug_title = new char[MAXLEN];
#endif

    Timings timings;
    timings.traj_total.set();

    do
    {
        try
        {
            timings.loop.set();
            gmx_rmpbc(gpbc, num_atoms, box, x0);

            timings.interface.set();
            const auto interface = find_interface_indices(
                x0, grpindex, grpsize, conf, pbc);
            timings.interface.stop();

            timings.bottom.set();
            const auto bottom = find_bottom_layer_indices(x0, interface, conf);
            timings.bottom.stop();

            Interface current {x0, num_atoms, bottom, interface};

            timings.contact_line.set();
            add_contact_line(current, x0, bottom, conf);
            timings.contact_line.stop();

            interfaces.push_back(current);

            if (interfaces.size() > conf.stride)
            {
                const auto& previous = interfaces.front();
                const auto inds_advanced = contact_line_advancements(
                    current, previous, conf);
                const auto from_previous = at_previous_contact_line(
                    inds_advanced, current, previous, conf);

                analyze_replacement(inds_advanced, current, previous, conf);

                num_advanced.push_back(inds_advanced.size());
                num_from_previous.push_back(from_previous.size());
                times.push_back(t);

                interfaces.pop_front();

#ifdef DEBUG_CONTACTLINE
                if (inds_advanced.size() > 0)
                {
                    const auto fraction =
                        static_cast<real>(num_from_previous.back())
                        / static_cast<real>(num_advanced.back());
                    fprintf(stderr, "%lu of %lu (%.2f) advancements were hops.\n",
                            num_from_previous.back(),
                            num_advanced.back(),
                            fraction);
                }
                else
                {
                    fprintf(stderr, "No advancement was made.\n");
                }
#endif
            }

#ifdef DEBUG_CONTACTLINE
            vector<int> cl_inds;
            for (auto i : current.contact_line)
            {
                cl_inds.push_back(i);
            }

            snprintf(debug_filename, MAXLEN, "contact_line_%05.1fps.gro", t);
            snprintf(debug_title, MAXLEN, "contact_line");
            write_sto_conf_indexed(debug_filename, debug_title,
                                   &top->atoms, x0, NULL, ePBC, box,
                                   cl_inds.size(),
                                   cl_inds.data());

            snprintf(debug_filename, MAXLEN, "interface_%05.1fps.gro", t);
            snprintf(debug_title, MAXLEN, "interface");
            write_sto_conf_indexed(debug_filename, debug_title,
                                   &top->atoms, x0, NULL, ePBC, box,
                                   interface.size(),
                                   interface.data());
#endif
            timings.loop.stop();
        }
        GMX_CATCH_ALL_AND_EXIT_WITH_FATAL_ERROR;
    }
    while (read_next_x(oenv, status, &t, x0, box));
    gmx_rmpbc_done(gpbc);

    timings.traj_total.stop();
    print_timings(timings);

#ifdef DEBUG_CONTACTLINE
    delete[] debug_filename;
    delete[] debug_title;
#endif

    close_trj(status);
    fprintf(stderr, "\nRead %d frames from trajectory.\n", 0);
    sfree(x0);
    delete pbc;

    const auto fractions = calculate_fractions(num_from_previous, num_advanced);

    const CLData result {
        times: times,
        fractions: fractions
    };

    return result;
}

static void
save_contact_line_figure(const CLData           &data,
                         const char             *filename,
                         const gmx_output_env_t *oenv)
{
    const auto title = "Contact line advancement data";
    const auto xlabel = "Time (ps)";
    const auto ylabel = "Fraction";

    auto file = xvgropen(filename, title, xlabel, ylabel, oenv);
    auto t = data.times.cbegin();
    auto f = data.fractions.cbegin();

    while (t != data.times.cend())
    {

        fprintf(file, "%12.3f %1.3f\n", *t++, *f++);
    }

    xvgrclose(file);
}

int
gmx_contact_line(int argc, char *argv[])
{
    const char *desc[] = {
        "[THISMODULE] analyzes molecules at the contact line.",
        "",
    };

    static rvec rmin = { 0.0,  0.0,  0.0},
                rmax = {-1.0, -1.0, -1.0};
    static int nmin = 20,
               nmax = 100,
               stride = 1;
    static real cutoff = 1.0,
                precision = 0.3,
                dx = 0.3,
                hop_min = 0.25,
                hop_max = 0.35;
    const char *algorithm[] = { NULL, "contact-line", "bottom", "hops", NULL };

    t_pargs pa[] = {
        { "-rmin", FALSE, etRVEC, { rmin },
          "Minimum coordinate values for atom positions to include." },
        { "-rmax", FALSE, etRVEC, { rmax },
          "Maximum coordinate values for atom positions to include." },
        { "-co", FALSE, etREAL, { &cutoff },
          "Cutoff distance for neighbour search." },
        { "-nmin", FALSE, etINT, { &nmin },
          "Minimum number of atoms within cutoff." },
        { "-nmax", FALSE, etINT, { &nmax },
          "Maximum number of atoms within cutoff." },
        { "-prec", FALSE, etREAL, { &precision },
          "Precision of slices along y and z." },
        { "-dx", FALSE, etREAL, { &dx },
          "Minimum distance for contact line advancement along x." },
        { "-hmin", FALSE, etREAL, { &hop_min },
          "Minimum distance for a hop to the contact line." },
        { "-hmax", FALSE, etREAL, { &hop_max },
          "Maximum distance for a hop to the contact line." },
        { "-stride", FALSE, etINT, { &stride },
          "Stride between contact line comparisons." },
        { "-al" , FALSE, etENUM, { &algorithm },
          "Algorithm for determining elegibility of atoms." },
    };

    const char *bugs[] = {
    };

    t_filenm fnm[] = {
        { efTRX, "-f", NULL,  ffREAD },
        { efNDX, NULL, NULL,  ffOPTRD },
        { efTPR, NULL, NULL,  ffREAD },
        { efXVG, "-o", "clfrac", ffWRITE },
    };

#define NFILE asize(fnm)

    gmx_output_env_t *oenv;
    if (!parse_common_args(&argc, argv, PCA_CAN_TIME | PCA_CAN_VIEW,
                           NFILE, fnm, asize(pa), pa, asize(desc), desc, asize(bugs), bugs,
                           &oenv))
    {
        return 0;
    }

    int ePBC;
    const auto top = read_top(ftp2fn(efTPR, NFILE, fnm), &ePBC);

    char **grpnames;
    int   *grpsizes;
    int  **index;

    snew(index, 1);
    snew(grpnames, 1);
    snew(grpsizes, 1);

    fprintf(stderr, "\nSelect group to analyze:\n");
    get_index(&top->atoms, ftp2fn_null(efNDX, NFILE, fnm),
              1, grpsizes, index, grpnames);

    struct CLConf conf {pa, asize(pa), rmin, rmax, nenum(algorithm)};

    const auto contact_line_data = collect_contact_line_advancement(
        ftp2fn(efTRX, NFILE, fnm), *index, *grpsizes, conf, top, ePBC, oenv);
    save_contact_line_figure(contact_line_data, opt2fn("-o", NFILE, fnm), oenv);

    view_all(oenv, NFILE, fnm);

    sfree(index);
    sfree(grpnames);
    sfree(grpsizes);

    return 0;
}
