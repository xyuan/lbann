////////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2014-2016, Lawrence Livermore National Security, LLC.
// Produced at the Lawrence Livermore National Laboratory.
// Written by the LBANN Research Team (B. Van Essen, et al.) listed in
// the CONTRIBUTORS file. <lbann-dev@llnl.gov>
//
// LLNL-CODE-697807.
// All rights reserved.
//
// This file is part of LBANN: Livermore Big Artificial Neural Network
// Toolkit. For details, see http://software.llnl.gov/LBANN or
// https://github.com/LLNL/LBANN.
//
// Licensed under the Apache License, Version 2.0 (the "Licensee"); you
// may not use this file except in compliance with the License.  You may
// obtain a copy of the License at:
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the license.
//
////////////////////////////////////////////////////////////////////////////////

#include "lbann/data_readers/lbann_data_reader_mnist.hpp"
#include "lbann/callbacks/lbann_callback_save_images.hpp"
#include "lbann/lbann.hpp"

using namespace std;
using namespace lbann;
using namespace El;


// layer definition
/*const std::vector<int> g_LayerDim = {784, 100, 30, 10};
const uint g_NumLayers = g_LayerDim.size(); // # layers*/

/// Main function
int main(int argc, char* argv[])
{
    // El initialization (similar to MPI_Init)
    Initialize(argc, argv);
    init_random(42);
    init_data_seq_random(42);
    lbann_comm* comm = NULL;

    try {

        // Get data files
        const string g_MNIST_TrainLabelFile = Input("--train-label-file",
                                                    "MNIST training set label file",
                                                    std::string("train-labels-idx1-ubyte"));
        const string g_MNIST_TrainImageFile = Input("--train-image-file",
                                                    "MNIST training set image file",
                                                    std::string("train-images-idx3-ubyte"));
        const string g_MNIST_TestLabelFile = Input("--test-label-file",
                                                   "MNIST test set label file",
                                                   std::string("t10k-labels-idx1-ubyte"));
        const string g_MNIST_TestImageFile = Input("--test-image-file",
                                                   "MNIST test set image file",
                                                   std::string("t10k-images-idx3-ubyte"));

        ///////////////////////////////////////////////////////////////////
        // initalize grid, block
        ///////////////////////////////////////////////////////////////////

        // Initialize parameter defaults
        TrainingParams trainParams;
        trainParams.DatasetRootDir = "/p/lscratchf/brainusr/datasets/MNIST/";
        trainParams.EpochCount = 50;
        trainParams.MBSize = 192;
        trainParams.LearnRate = 0.01;
        trainParams.DropOut = -1.0f;
        trainParams.ProcsPerModel = 0;
        trainParams.PercentageTrainingSamples = 0.90;
        trainParams.PercentageValidationSamples = 1.00;
        PerformanceParams perfParams;
        perfParams.BlockSize = 256;

        // Parse command-line inputs
        trainParams.parse_params();
        perfParams.parse_params();
        ProcessInput();
        PrintInputReport();

        // Set algorithmic blocksize
        SetBlocksize(perfParams.BlockSize);

        // Set up the communicator and get the grid.
        comm = new lbann_comm(trainParams.ProcsPerModel);
        Grid& grid = comm->get_model_grid();
        if (comm->am_world_master()) {
          cout << "Number of models: " << comm->get_num_models() << endl;
          cout << "Grid is " << grid.Height() << " x " << grid.Width() << endl;
          cout << endl;
        }

        int parallel_io = perfParams.MaxParIOSize;
        if (parallel_io == 0) {
          if (comm->am_world_master()) {
            cout << "\tMax Parallel I/O Fetch: " << comm->get_procs_per_model() <<
              " (Limited to # Processes)" << endl;
          }
          parallel_io = comm->get_procs_per_model();
        } else {
          if (comm->am_world_master()) {
            cout << "\tMax Parallel I/O Fetch: " << parallel_io << endl;
          }
        }

        ///////////////////////////////////////////////////////////////////
        // load training data (MNIST)
        ///////////////////////////////////////////////////////////////////
        DataReader_MNIST mnist_trainset(trainParams.MBSize, true);
        mnist_trainset.set_file_dir(trainParams.DatasetRootDir);
        mnist_trainset.set_data_filename(g_MNIST_TrainImageFile);
        mnist_trainset.set_use_percent(trainParams.PercentageTrainingSamples);
        mnist_trainset.load();
        if (comm->am_world_master()) {
          cout << "Training using " << (trainParams.PercentageTrainingSamples*100) << "% of the training data set, which is " << mnist_trainset.getNumData() << " samples." << endl;
        }

        ///////////////////////////////////////////////////////////////////
        // create a validation set from the unused training data (MNIST)
        ///////////////////////////////////////////////////////////////////
        DataReader_MNIST mnist_validation_set(mnist_trainset); // Clone the training set object
        if (!mnist_validation_set.swap_used_and_unused_index_sets()) { // Swap the used and unused index sets so that it validates on the remaining data
          if (comm->am_world_master()) {
            cout << "MNIST validation data error" << endl;
          }
          return -1;
        }

        if(trainParams.PercentageValidationSamples == 1.00) {
          if (comm->am_world_master()) {
            cout << "Validating training using " << ((1.00 - trainParams.PercentageTrainingSamples)*100) << "% of the training data set, which is " << mnist_validation_set.getNumData() << " samples." << endl;
          }
        }else {
          size_t preliminary_validation_set_size = mnist_validation_set.getNumData();
          size_t final_validation_set_size = mnist_validation_set.trim_data_set(trainParams.PercentageValidationSamples);
          if (comm->am_world_master()) {
            cout << "Trim the validation data set from " << preliminary_validation_set_size << " samples to " << final_validation_set_size << " samples." << endl;
          }
        }

        ///////////////////////////////////////////////////////////////////
        // load testing data (MNIST)
        ///////////////////////////////////////////////////////////////////
        DataReader_MNIST mnist_testset(trainParams.MBSize, true);
        mnist_testset.set_file_dir(trainParams.DatasetRootDir);
        mnist_testset.set_data_filename(g_MNIST_TestImageFile);
        mnist_testset.set_label_filename(g_MNIST_TestLabelFile);
        mnist_testset.set_use_percent(trainParams.PercentageTestingSamples);
        mnist_testset.load();
        if (comm->am_world_master()) {
          cout << "Testing using " << (trainParams.PercentageTestingSamples*100) << "% of the testing data set, which is " << mnist_testset.getNumData() << " samples." << endl;
        }

        ///////////////////////////////////////////////////////////////////
        // initalize neural network (layers)
        ///////////////////////////////////////////////////////////////////

        // Initialize optimizer
        Optimizer_factory *optimizer;
        if (trainParams.LearnRateMethod == 1) { // Adagrad
          optimizer = new Adagrad_factory(comm, trainParams.LearnRate);
        }else if (trainParams.LearnRateMethod == 2) { // RMSprop
          optimizer = new RMSprop_factory(comm/*, trainParams.LearnRate*/);
        }else {
          optimizer = new SGD_factory(comm, trainParams.LearnRate, 0.9, trainParams.LrDecayRate, true);
        }

        // Initialize network
        layer_factory* lfac = new layer_factory();
        greedy_layerwise_autoencoder gla(trainParams.MBSize, comm, new objective_functions::mean_squared_error(comm), lfac, optimizer);
        std::map<execution_mode, DataReader*> data_readers = {std::make_pair(execution_mode::training,&mnist_trainset),
                                                               std::make_pair(execution_mode::validation, &mnist_validation_set),
                                                               std::make_pair(execution_mode::testing, &mnist_testset)};

        input_layer *input_layer = new input_layer_distributed_minibatch_parallel_io(data_layout::MODEL_PARALLEL, comm, parallel_io, (int) trainParams.MBSize, data_readers);
        gla.add(input_layer);
        gla.add("FullyConnected", data_layout::MODEL_PARALLEL, 32, trainParams.ActivationType, weight_initialization::glorot_uniform, {new dropout(data_layout::MODEL_PARALLEL, comm, trainParams.DropOut)});

        if (comm->am_world_master()) {
          cout << "Parameter settings:" << endl;
          cout << "\tMini-batch size: " << trainParams.MBSize << endl;
          cout << "\tLearning rate: " << trainParams.LearnRate << endl;
          cout << "\tEpoch count: " << trainParams.EpochCount << endl;
        }

        ///////////////////////////////////////////////////////////////////
        // main loop for training/testing
        ///////////////////////////////////////////////////////////////////

        // Initialize the model's data structures
        gla.setup();

        // set checkpoint directory and checkpoint interval
        // @TODO: add to lbann_proto
        gla.set_checkpoint_dir(trainParams.ParameterDir);
        gla.set_checkpoint_epochs(trainParams.CkptEpochs);
        gla.set_checkpoint_steps(trainParams.CkptSteps);
        gla.set_checkpoint_secs(trainParams.CkptSecs);

        // restart model from checkpoint if we have one
        gla.restartShared();

        if (comm->am_world_master()) cout << "(Pre) train autoencoder - unsupersived training" << endl;
        gla.train(trainParams.EpochCount);


        // Free dynamically allocated memory
        // delete target_layer;  // Causes segfault
        // delete input_layer;  // Causes segfault
        // delete lfac;  // Causes segfault
        delete optimizer;
        delete comm;

    }
    catch (lbann_exception& e) { lbann_report_exception(e, comm); }
    catch (exception& e) { ReportException(e); } /// Elemental exceptions

    // free all resources by El and MPI
    Finalize();

    return 0;
}
