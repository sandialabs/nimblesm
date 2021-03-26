/*
//@HEADER
// ************************************************************************
//
//                                NimbleSM
//                             Copyright 2018
//   National Technology & Engineering Solutions of Sandia, LLC (NTESS)
//
// Under the terms of Contract DE-NA0003525 with NTESS, the U.S. Government
// retains certain rights in this software.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the Corporation nor the names of the
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY NTESS "AS IS" AND ANY EXPRESS OR IMPLIED
// WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN
// NO EVENT SHALL NTESS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
// TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Questions?  Contact David Littlewood (djlittl@sandia.gov)
//
// ************************************************************************
//@HEADER
*/

#include "nimble_defs.h"
#include "nimble_contact_interface.h"
#include "nimble_contact_manager.h"
#include "nimble_data_manager.h"

#include "nimble_kokkos.h"
#include "nimble.quanta.stopwatch.h"
#include "nimble_kokkos_block.h"
#include "nimble_kokkos_material_factory.h"
#include "nimble_kokkos_model_data.h"
#include "nimble_kokkos_profiling.h"
#include "nimble_parser.h"
#include "nimble_timer.h"
#include "nimble_timing_utils.h"
#include "nimble_utils.h"
#include "nimble_version.h"
#include "nimble_view.h"
#include "nimble_kokkos_block_material_interface_factory.h"

#ifdef NIMBLE_HAVE_ARBORX
#ifdef NIMBLE_HAVE_MPI
    #include "contact/parallel/arborx_parallel_contact_manager.h"
  #endif
    #include "contact/serial/arborx_serial_contact_manager.h"
#endif

#include <cassert>
#include <iostream>

namespace nimble {

namespace details_kokkos {

int ExplicitTimeIntegrator(const nimble::Parser & parser,
                           nimble::GenesisMesh & mesh,
                           nimble::DataManager & data_manager,
                           std::shared_ptr<nimble::ContactInterface> contact_interface
);

int parseCommandLine(int argc, char **argv, nimble::Parser &parser)
{
  for (int ia = 1; ia < argc; ++ia) {
    std::string my_arg = std::string(argv[ia]);
    if (my_arg == "--use_vt") {
#ifdef NIMBLE_HAVE_VT
      parser.SetToUseVT();
#else
      std::cerr << "\n Flag '--use_vt' ignored \n\n";
#endif
      continue;
    }
    if (my_arg == "--use_kokkos") {
#ifdef NIMBLE_HAVE_KOKKOS
      parser.SetToUseKokkos();
#else
      std::cerr << "\n Flag '--use_kokkos' ignored \n\n";
#endif
      continue;
    }
    if (my_arg == "--use_tpetra") {
#ifdef NIMBLE_HAVE_TRILINOS
      parser.SetToUseTpetra();
#else
      std::cerr << "\n Flag '--use_tpetra' ignored \n\n";
#endif
      continue;
    }
    //
    parser.SetInputFilename(std::string(my_arg));
  }
  return 0;
}

}


void NimbleKokkosInitializeAndGetInput(int argc, char **argv, nimble::Parser &parser)
{

  int my_rank = 0, num_ranks = 1;

  // --- Parse the command line
  details_kokkos::parseCommandLine(argc, argv, parser);

#ifdef NIMBLE_HAVE_TRILINOS
  if (parser.UseTpetra()) {
    auto sguard = new Tpetra::ScopeGuard(&argc,&argv);
    parser.ResetTpetraScope(sguard);
    auto comm = Tpetra::getDefaultComm();
    num_ranks = comm->getSize();
    my_rank = comm->getRank();
  }
  else
#endif
#ifdef NIMBLE_HAVE_MPI
  {
    MPI_Init(&argc, &argv);
    int mpi_err = MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
    if (mpi_err != MPI_SUCCESS) {
      throw std::logic_error(
          "\nError:  MPI_Comm_rank() returned nonzero error code.\n");
    }
    mpi_err = MPI_Comm_size(MPI_COMM_WORLD, &num_ranks);
    if (mpi_err != MPI_SUCCESS) {
      throw std::logic_error(
          "\nError:  MPI_Comm_size() returned nonzero error code.\n");
    }
  }
#endif

#ifdef NIMBLE_HAVE_KOKKOS
  Kokkos::initialize(argc, argv);
#endif

  if (argc < 2) {
    if (my_rank == 0) {
#ifdef NIMBLE_HAVE_MPI
      std::cout << "Usage:  mpirun -np NP NimbleSM <input_deck.in>\n" << std::endl;
#else
      std::cout << "Usage:  NimbleSM <input_deck.in>\n" << std::endl;
#endif
    }
    throw std::runtime_error("\nError: Inappropriate set of parameters.\n");
  }

  // Banner
  if (my_rank == 0) {
    std::cout << "\n-- NimbleSM" << std::endl;
    std::cout << "-- version " << nimble::NimbleVersion() << "\n";
    if (parser.UseKokkos()) {
      std::cout << "-- Using Kokkos interface \n";
    }
    else if (parser.UseTpetra()) {
      std::cout << "-- Using Tpetra interface \n";
    }
    else if (parser.UseVT()) {
      std::cout << "-- Using VT runtime \n";
    }
    std::cout << "-- Number of rank";
    if (num_ranks > 1)
      std::cout << "(s)";
    std::cout << " = " << num_ranks << "\n";
    std::cout << std::endl;
  }

  // Initialize VT if needed
#ifdef NIMBLE_HAVE_VT
  if (parser.UseVT() == true) {
    MPI_Comm vt_comm = MPI_COMM_WORLD;
    ::vt::CollectiveOps::initialize(argc, argv, ::vt::no_workers, true, &vt_comm );
  }
#endif

  parser.SetRankID(my_rank);
  parser.SetNumRanks(num_ranks);

  parser.Initialize();

}


void NimbleKokkosMain(const std::shared_ptr<MaterialFactoryType>& material_factory,
                      std::shared_ptr<nimble::ContactInterface> contact_interface,
                      const std::shared_ptr<nimble::BlockMaterialInterfaceFactoryBase> &block_material,
                      const nimble::Parser &parser)
{

  const int my_rank = parser.GetRankID();
  const int num_ranks = parser.GetNumRanks();

  //--- Define timers
  nimble_kokkos::ProfilingTimer watch_simulation;
  watch_simulation.push_region("Parse and read mesh");

  // Read the mesh
  nimble::GenesisMesh mesh;
  nimble::GenesisMesh rve_mesh;
  {
    //--- This part is independent of Kokkos
    std::string genesis_file_name = nimble::IOFileName(parser.GenesisFileName(), "g", "", my_rank, num_ranks);
    std::string rve_genesis_file_name = nimble::IOFileName(parser.RVEGenesisFileName(), "g");
    mesh.ReadFile(genesis_file_name);
    if (rve_genesis_file_name != "none") {
      rve_mesh.ReadFile(rve_genesis_file_name);
    }
  }

  nimble::DataManager data_manager(parser, mesh, rve_mesh);
  data_manager.SetBlockMaterialInterfaceFactory(block_material);

  watch_simulation.pop_region_and_report_time();

#ifdef NIMBLE_HAVE_ARBORX
  std::string tag = "arborx";
#else
  std::string tag = "kokkos";
#endif
  std::string output_exodus_name = nimble::IOFileName(parser.ExodusFileName(), "e", tag, my_rank, num_ranks);
  const int dim = mesh.GetDim();
  const int num_nodes = static_cast<int>(mesh.GetNumNodes());

  if (my_rank == 0) {
    std::cout << "\n";
    if (num_ranks == 1) {
      std::cout << " Number of Nodes = " << num_nodes << "\n";
      std::cout << " Number of Elements = " << mesh.GetNumElements() << "\n";
    }
    std::cout << " Number of Global Blocks = " << mesh.GetNumGlobalBlocks() << "\n";
    std::cout << "\n";
    std::cout << " Number of Ranks         = " << num_ranks << "\n";
#ifdef _OPENMP
    std::cout << " Number of Threads       = " << omp_get_max_threads() << "\n";
#endif
    std::cout << "\n";
  }
  watch_simulation.push_region("Model data and field allocation");

  auto macroscale_data = data_manager.GetMacroScaleData();
  macroscale_data->InitializeBlocks(data_manager, material_factory);

  //
  // Initialize the output file
  //

  data_manager.InitializeOutput(output_exodus_name);

  watch_simulation.pop_region_and_report_time();

  const auto &time_integration_scheme = parser.TimeIntegrationScheme();
  if (time_integration_scheme == "explicit") {
    details_kokkos::ExplicitTimeIntegrator(parser, mesh, data_manager,
                                           contact_interface);
  }
  else {
    throw std::runtime_error("\n Time Integration Scheme Not Implemented \n");
  }

}


int NimbleKokkosFinalize(const nimble::Parser &parser) {

#ifdef NIMBLE_HAVE_VT
  while ( !::vt::curRT->isTerminated() )
      ::vt::runScheduler();
#endif

#ifdef NIMBLE_HAVE_KOKKOS
  if (parser.UseKokkos())
    Kokkos::finalize();
#endif

#ifdef NIMBLE_HAVE_TRILINOS
  if (!parser.UseTpetra()) {
#ifdef NIMBLE_HAVE_MPI
    MPI_Finalize();
#endif
  }
#else
  #ifdef NIMBLE_HAVE_MPI
    MPI_Finalize();
  #endif
#endif

  return 0;

}


namespace details_kokkos {

int ExplicitTimeIntegrator(const nimble::Parser & parser,
                           nimble::GenesisMesh & mesh,
                           nimble::DataManager & data_manager,
                           std::shared_ptr<nimble::ContactInterface> contact_interface
)
{
  const int my_rank = parser.GetRankID();
  const int num_ranks = parser.GetNumRanks();

  auto num_nodes = static_cast<int>(mesh.GetNumNodes());

  auto *model_data_ptr = dynamic_cast<nimble_kokkos::ModelData*>(data_manager.GetMacroScaleData().get());
  if (model_data_ptr == nullptr) {
    throw std::runtime_error(" Incompatible Model Data \n");
  }
  nimble_kokkos::ModelData &model_data = *model_data_ptr;

  nimble_kokkos::ProfilingTimer watch_simulation;
  watch_simulation.push_region("Lumped mass gather and compute");

  auto reference_coordinate = model_data.GetVectorNodeData("reference_coordinate");

  auto velocity = model_data.GetVectorNodeData("velocity");
  velocity.zero();

  auto acceleration = model_data.GetVectorNodeData("acceleration");
  acceleration.zero();

  model_data.ComputeLumpedMass(data_manager);

  auto lumped_mass = model_data.GetScalarNodeData("lumped_mass");

  auto displacement = model_data.GetVectorNodeData("displacement");
  displacement.zero();

  auto internal_force = model_data.GetVectorNodeData("internal_force");
  internal_force.zero();

  auto contact_force = model_data.GetVectorNodeData("contact_force");
  contact_force.zero();

  double critical_time_step = model_data.GetCriticalTimeStep();

  watch_simulation.push_region("Contact setup");

  auto contact_manager = nimble::GetContactManager(contact_interface, data_manager);

  auto myVectorCommunicator = data_manager.GetVectorCommunicator();

  const bool contact_enabled = parser.HasContact();
  const bool contact_visualization = parser.ContactVisualization();

  if (contact_enabled) {
    std::vector<std::string> contact_primary_block_names, contact_secondary_block_names;
    double penalty_parameter;
    nimble::ParseContactCommand(parser.ContactString(),
                                contact_primary_block_names,
                                contact_secondary_block_names,
                                penalty_parameter);
    std::vector<int> contact_primary_block_ids, contact_secondary_block_ids;
    mesh.BlockNamesToOnProcessorBlockIds(contact_primary_block_names,
                                         contact_primary_block_ids);
    mesh.BlockNamesToOnProcessorBlockIds(contact_secondary_block_names,
                                         contact_secondary_block_ids);
    contact_manager->SetPenaltyParameter(penalty_parameter);
    contact_manager->CreateContactEntities(mesh,
                                          *myVectorCommunicator,
                                          contact_primary_block_ids,
                                          contact_secondary_block_ids);
    if (contact_visualization) {
#ifdef NIMBLE_HAVE_ARBORX
      std::string tag = "arborx";
#else
      std::string tag = "kokkos";
#endif
      std::string contact_visualization_exodus_file_name = nimble::IOFileName(parser.ContactVisualizationFileName(), "e", tag, my_rank, num_ranks);
      contact_manager->InitializeContactVisualization(contact_visualization_exodus_file_name);
    }
  }

  watch_simulation.pop_region_and_report_time();

  watch_simulation.push_region("BC enforcement");

  double time_current(0.0), time_previous(0.0);
  double final_time = parser.FinalTime();
  double delta_time(0.0), half_delta_time(0.0);
  const int num_load_steps = parser.NumLoadSteps();
  int output_frequency = parser.OutputFrequency();

  model_data.ApplyInitialConditions(data_manager);
  model_data.ApplyKinematicConditions(data_manager, time_current, time_previous);

  watch_simulation.pop_region_and_report_time();

  // Output to Exodus file
  watch_simulation.push_region("Output");

  data_manager.WriteOutput(time_current);

  if (contact_visualization) {
    contact_manager->ContactVisualizationWriteStep(time_current);
  }

  watch_simulation.pop_region_and_report_time();
  //
  double total_internal_force_time = 0.0, total_contact_time = 0.0;
  double total_contact_getf = 0.0;
  double total_vector_reduction_time = 0.0;
  double total_update_avu_time = 0.0;
  double total_exodus_write_time = 0.0;
  //
  std::map<int, std::size_t> contactInfo;
  //
  watch_simulation.push_region("Time stepping loop");
  nimble_kokkos::ProfilingTimer watch_internal;
  constexpr int mpi_vector_dim = 3;

  for (int step = 0 ; step < num_load_steps ; ++step) {

    if (my_rank == 0) {
      if (10*(step+1) % num_load_steps == 0 && step != num_load_steps - 1) {
        std::cout << "   " << static_cast<int>( 100.0 * static_cast<double>(step+1)/num_load_steps )
                  << "% complete" << std::endl << std::flush;
      }
      else if (step == num_load_steps - 1) {
        std::cout << "  100% complete\n" << std::endl << std::flush;
      }
    }
    bool is_output_step = (step%output_frequency == 0 || step == num_load_steps - 1);

    watch_internal.push_region("Central difference");
    time_previous = time_current;
    time_current += final_time/num_load_steps;
    delta_time = time_current - time_previous;
    half_delta_time = 0.5*delta_time;

    // V^{n+1/2} = V^{n} + (dt/2) * A^{n}
    velocity += half_delta_time * acceleration;
    total_update_avu_time += watch_internal.pop_region_and_report_time();

    // Apply kinematic boundary conditions
    watch_internal.push_region("BC enforcement");
    model_data.ApplyKinematicConditions(data_manager, time_current, time_previous);
    watch_internal.pop_region_and_report_time();

    // U^{n+1} = U^{n} + (dt)*V^{n+1/2}
    watch_internal.push_region("Central difference");
    displacement += delta_time * velocity;

    // Copy the current displacement and velocity value to device memory

    total_update_avu_time += watch_internal.pop_region_and_report_time();

    nimble_kokkos::ProfilingTimer watch_internal_details;

    watch_internal.push_region("Force calculation");
    model_data.ComputeInternalForce(data_manager, time_previous, time_current,
                                    is_output_step, displacement, internal_force);

    //
    // Evaluate the contact force
    //
    if (contact_enabled) {
      watch_internal_details.push_region("Contact");
      contact_manager->ComputeContactForce(step+1, is_output_step,
                                           contact_force);
      total_contact_time += watch_internal_details.pop_region_and_report_time();
      //
      auto tmpNum = contact_manager->numActiveContactFaces();
      if (tmpNum)
        contactInfo.insert(std::make_pair(step, tmpNum));
      //
//      if (contact_visualization && is_output_step)
//        contact_manager->ContactVisualizationWriteStep(time_current);
    }

    // fill acceleration vector A^{n+1} = M^{-1} ( F^{n} + b^{n} )
    watch_internal.push_region("Central difference");
    if (contact_enabled) {
      for (int i = 0; i < num_nodes; ++i) {
        acceleration(i, 0) = (1.0 / lumped_mass(i)) *
                               (internal_force(i, 0) + contact_force(i, 0));
        acceleration(i, 1) = (1.0 / lumped_mass(i)) *
                               (internal_force(i, 1) + contact_force(i, 1));
        acceleration(i, 2) = (1.0 / lumped_mass(i)) *
                               (internal_force(i, 2) + contact_force(i, 2));
      }
    }
    else {
      for (int i = 0; i < num_nodes; ++i) {
        acceleration(i, 0) = (1.0 / lumped_mass(i)) * internal_force(i, 0);
        acceleration(i, 1) = (1.0 / lumped_mass(i)) * internal_force(i, 1);
        acceleration(i, 2) = (1.0 / lumped_mass(i)) * internal_force(i, 2);
      }
    }

    // V^{n+1}   = V^{n+1/2} + (dt/2)*A^{n+1}
    velocity += half_delta_time * acceleration;
    total_update_avu_time += watch_internal.pop_region_and_report_time();

    if (is_output_step) {
      //
      watch_internal.push_region("Output");
      model_data.ApplyKinematicConditions(data_manager, time_current, time_previous);
      data_manager.WriteOutput(time_current);
      //
      if (contact_visualization) {
        contact_manager->ContactVisualizationWriteStep(time_current);
      }

      total_exodus_write_time += watch_internal.pop_region_and_report_time();
    }

    watch_internal.push_region("Copy field data new to old");
    model_data.UpdateStates(data_manager);
    watch_internal.pop_region_and_report_time();

  } // loop over time steps
  double total_simulation_time = watch_simulation.pop_region_and_report_time();

  for (int irank = 0; irank < num_ranks; ++irank) {
#ifdef NIMBLE_HAVE_MPI
    MPI_Barrier(MPI_COMM_WORLD);
#endif
    if ((my_rank == irank) && (!contactInfo.empty())) {
      std::cout << " Rank " << irank << " has " << contactInfo.size()
                << " contact entries "
                << "(out of " << num_load_steps << " time steps)."<< std::endl;
      std::cout.flush();
    }
#ifdef NIMBLE_HAVE_MPI
    MPI_Barrier(MPI_COMM_WORLD);
#endif
  }

  if (my_rank == 0 && parser.WriteTimingDataFile()) {
//    double tcontact = total_contact_applyd + total_arborx_time + total_contact_getf;
    nimble::TimingInfo timing_writer{
        num_ranks,
        nimble::quanta::stopwatch::get_microsecond_timestamp(),
        total_simulation_time,
        total_internal_force_time,
        total_contact_time,
        total_exodus_write_time,
        total_vector_reduction_time
    };
    timing_writer.BinaryWrite();
  }

  if (my_rank == 0) {
    std::cout << " Total Time Loop = " << total_simulation_time << "\n";
    std::cout << " --- Internal Forces = " << total_internal_force_time << "\n";
    if (contact_enabled) {
//      double tcontact = total_contact_applyd + total_arborx_time + total_contact_getf;
      std::cout << " --- Contact = " << total_contact_time << "\n";
//      std::cout << " --- >>> Apply displ. = " << total_contact_applyd << "\n";
//      std::cout << " --- >>> Search / Project / Enforce = " << total_arborx_time << "\n";
      auto list_timers = contact_manager->getTimers();
      for (const auto& st_pair : list_timers)
        std::cout << " --- >>> >>> " << st_pair.first << " = " << st_pair.second << "\n";
      std::cout << " --- >>> Get Forces = " << total_contact_getf << "\n";
    }
    std::cout << " --- Exodus Write = " << total_exodus_write_time << "\n";
    std::cout << " --- Update AVU = " << total_update_avu_time << "\n";
    std::cout << " --- Vector Reduction = " << total_vector_reduction_time << "\n";
  }

  return 0;
}

}


}