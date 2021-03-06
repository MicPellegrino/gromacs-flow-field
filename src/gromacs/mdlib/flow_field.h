#ifndef MD_FLOW_FIELD
#define MD_FLOW_FIELD

#include <array>
#include <string>
#include <vector>

#include "gromacs/mdtypes/state.h"

constexpr char FLOW_FILE_HEADER_NAME[] = "GMX_FLOW_2";

// We are using a grid along X and Z so we use a separate enum 
// to not confuse our indexing with regular XX, YY and ZZ
enum class GridAxes {
    X,
    Z,
    NumAxes
};
constexpr size_t NUM_FLOW_AXES = static_cast<size_t>(GridAxes::NumAxes);

// Indices for different data in array
enum class FlowVariable {
    NumAtoms,
    Temp,
    Mass,   // Mass in bin (amu)
    U,      // Velocity along X
    V,      //            and Z
    NumVariables
};

/* MICHELE */
/* Temporary flow data (i.e. live only for one time step)
 */
enum class TempVariable {
    Mass,	// Mass in bin (amu)
    Momx,   	// Momentum along X
    Momz,   	//	      and Z
    NumVariables
};

constexpr size_t NUM_FLOW_VARIABLES = static_cast<size_t>(FlowVariable::NumVariables);
constexpr size_t NUM_TEMP_VARIABLES = static_cast<size_t>(TempVariable::NumVariables);

struct GroupFlowData {
    std::string fnbase, 
                name;
    std::vector<double> data;

    /* MICHELE */
    std::vector<double> temp_data;

    GroupFlowData(const std::string& fnbase_original, 
                  const std::string& group_name, 
                  const size_t num_data,
		  const size_t num_temp)
    :name { group_name },
     data(num_data, 0.0),
     temp_data(num_temp, 0.0)
    {
        fnbase.append(fnbase_original);
        fnbase.append("_");
        fnbase.append(group_name);
    }
};

class FlowData {
public:
    bool bDoFlowCollection = false;

    std::string fnbase;

    std::vector<double> data;   		// A 2D grid is represented by this 1D array
    
    /* MICHELE */
    std::vector<double> temp_data;

    std::vector<GroupFlowData> group_data; 	// Similar data for all separate atom groups
    
    /* MICHELE */
    /* Do we need temporary data for each group?
     */
    std::vector<GroupFlowData> temp_group_data;	// ???

    uint64_t step_collect = 0, 
             step_output = 0,
             step_ratio = 0;

    /* MICHELE */
    /* This class probably need a copy constructor
     */
    double bin_volume;
    double time_step;
    bool retrace;
    bool new_velocity_binning;

    FlowData() {}

    FlowData(const std::string fnbase,
             const std::vector<std::string> group_names,
             const size_t nx, 
             const size_t nz,
             const double dx,
             const double dy,
             const double dz,
             const uint64_t step_collect,
             const uint64_t step_output,
	     const double dt,
	     const bool rtx,
	     const bool nvb)
    :bDoFlowCollection { true },
     fnbase { fnbase },
     data(nx * nz * NUM_FLOW_VARIABLES, 0.0),
     
     /* MICHELE */
     temp_data(nx * nz * NUM_TEMP_VARIABLES, 0.0),
     
     step_collect { step_collect },
     step_output { step_output },
     step_ratio { static_cast<uint64_t>(step_output / step_collect) },
     bin_volume { dx * dy * dz },
     num_bins { nx, nz },
     bin_size { dx, dz },
     inv_bin_size { 1.0 / dx, 1.0 / dz },
     time_step { dt },
     retrace { rtx },
     new_velocity_binning { nvb }
     {
         for (const auto& name : group_names)
         {
	     /* MICHELE */
	     /* Need temp for each group, in principle
	      */
             group_data.push_back(GroupFlowData( fnbase, name, data.size(), temp_data.size() ));
         }
     }

    double dx() const { return bin_size[static_cast<size_t>(GridAxes::X)]; }
    double dz() const { return bin_size[static_cast<size_t>(GridAxes::Z)]; }

    double inv_dx() const { return inv_bin_size[static_cast<size_t>(GridAxes::X)]; }
    double inv_dz() const { return inv_bin_size[static_cast<size_t>(GridAxes::Z)]; }

    size_t nx() const { return num_bins[static_cast<size_t>(GridAxes::X)]; }
    size_t nz() const { return num_bins[static_cast<size_t>(GridAxes::Z)]; }

    size_t get_1d_index(const size_t ix, const size_t iz) const 
    { 
        return (iz * nx() + ix) * NUM_FLOW_VARIABLES;
    }

    /* MICHELE */
    /* I don't like this solution, but I cannot find a elegent way to do otherwise
     */
    size_t get_1d_index_temp(const size_t ix, const size_t iz) const
    {
	return (iz * nx() + ix) * NUM_TEMP_VARIABLES;
    }

    size_t get_xbin(const real x) const { return get_bin_from_position(x, nx(), inv_dx()); }
    size_t get_zbin(const real z) const { return get_bin_from_position(z, nz(), inv_dz()); }

    float get_x(const size_t ix) const { return get_position(ix, dx()); }
    float get_z(const size_t iz) const { return get_position(iz, dz()); }

    void reset_data() 
    { 
        
	data.assign(data.size(), 0.0);

        for (auto& group : group_data)
        {
            group.data.assign(group.data.size(), 0.0);
        }

    }

    /* MICHELE */
    /* Coverts momenta stored in temp_data to velocities stored in data
     */
    void add_velocity_to_bins() 
    {
	// const int num_groups = group_data.empty() ? 1 : group_data.size();
        for ( size_t i = 0; i<nx(); ++i )
	{
	    for ( size_t j = 0; j<nz(); ++j )
	    {
	        size_t bin      = get_1d_index(i, j);
		size_t temp_bin = get_1d_index_temp(i, j);
		auto mass = temp_data[temp_bin + static_cast<size_t>(TempVariable::Mass)];
		auto px =   temp_data[temp_bin + static_cast<size_t>(TempVariable::Momx)];
		auto pz =   temp_data[temp_bin + static_cast<size_t>(TempVariable::Momz)];
		data[bin + static_cast<size_t>(FlowVariable::U)] += mass > 0.0 ? px / mass : 0.0;
    		data[bin + static_cast<size_t>(FlowVariable::V)] += mass > 0.0 ? pz / mass : 0.0;
		/*
		for ( int index_group = 0; index_group < num_groups; ++index_group )
		{
			mass = group_data.at(index_group).temp_data[temp_bin + static_cast<size_t>(TempVariable::Mass)];
			px =   group_data.at(index_group).temp_data[temp_bin + static_cast<size_t>(TempVariable::Momx)];
			pz =   group_data.at(index_group).temp_data[temp_bin + static_cast<size_t>(TempVariable::Momz)];
			group_data.at(index_group).data[bin + static_cast<size_t>(FlowVariable::U)] += mass > 0.0 ? px / mass : 0.0;
    			group_data.at(index_group).data[bin + static_cast<size_t>(FlowVariable::V)] += mass > 0.0 ? pz / mass : 0.0;
		}
		*/
	    }
	}
	reset_temp_data();
    }

private:
    std::array<size_t, NUM_FLOW_AXES> num_bins;
    std::array<double, NUM_FLOW_AXES> bin_size,
                                      inv_bin_size;

    size_t get_bin_from_position(const real x, const size_t num_bins, const real inv_bin) const
    {
        
	/* MICHELE */
	// auto index = static_cast<int>(floor(x * inv_bin)) % static_cast<int>(num_bins);
	auto index = static_cast<int>(std::floor(x * inv_bin)) % static_cast<int>(num_bins);

        while (index < 0)
        {
            index += num_bins;
        }

        return index;
    }

    float get_position(const size_t index, const float bin_size) const
    {
        return (static_cast<float>(index) + 0.5) * bin_size;
    }

    /* MICHELE */
    /* This is never called ouside (and should never be), so it may have sense to make it private
     */
    void reset_temp_data() 
    { 
        temp_data.assign(temp_data.size(), 0.0);
	/*
	const int num_groups = group_data.empty() ? 1 : group_data.size();
	for ( int index_group = 0; index_group < num_groups; ++index_group )
	{
		group_data.at(index_group).temp_data.assign( group_data.at(index_group).temp_data.size(), 0.0 );
	}
	*/
    }
};

// Prepare and return a container for flow field data
FlowData
init_flow_container(const int               nfile,
                    const t_filenm          fnm[],
                    const t_inputrec       *ir,
                    const SimulationGroups *groups,
                    const t_state          *state,
		    const bool		    rtx,
		    const bool		    nvb);

// Write information about the flow field collection
void 
print_flow_collection_information(const FlowData &flowcr);

// If at a collection or output step, perform actions
void
flow_collect_or_output(FlowData               &flowcr,
                       const uint64_t          step,
                       const t_commrec        *cr,
                       const t_inputrec       *ir,
                       const t_mdatoms        *mdatoms,
                       const t_state          *state,
                       const SimulationGroups *groups);

#endif
