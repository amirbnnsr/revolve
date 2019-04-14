/*
 * Copyright (C) 2015-2018 Vrije Universiteit Amsterdam
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Description: TODO: <Add brief description about file purpose>
 * Author: Milan Jelisavcic & Maarten van Hooft
 * Date: December 29, 2018
 *
 */
#ifndef USE_NLOPT
#define USE_NLOPT
#endif

// STL macros
#include <cstdlib>
#include <map>
#include <algorithm>
#include <random>
#include <tuple>
#include <time.h>

// Other libraries
#include <limbo/acqui/ucb.hpp>
#include <limbo/acqui/gp_ucb.hpp>
#include <limbo/bayes_opt/bo_base.hpp>
#include <limbo/init/lhs.hpp>
#include <limbo/kernel/exp.hpp>
#include <limbo/model/gp.hpp>
#include <limbo/mean/mean.hpp>
#include <limbo/tools/macros.hpp>
#include <limbo/opt/nlopt_no_grad.hpp>

// Project headers
#include "../motors/Motor.h"

#include "../sensors/Sensor.h"

#include "DifferentialCPG.h"

#include "DifferentialCPG_BO.h"

// Define namespaces
namespace gz = gazebo;
using namespace revolve::gazebo;

// Probably not so nice
using Mean_t = limbo::mean::Data<DifferentialCPG::Params>;
using Kernel_t = limbo::kernel::Exp<DifferentialCPG::Params>;
using GP_t = limbo::model::GP<DifferentialCPG::Params, Kernel_t, Mean_t>;
using Init_t = limbo::init::LHS<DifferentialCPG::Params>;
using Acqui_t = limbo::acqui::UCB<DifferentialCPG::Params, GP_t>;

#ifndef USE_NLOPT
#define USE_NLOPT //installed NLOPT
#endif

/**
 * Constructor for DifferentialCPG class.
 *
 * @param _model
 * @param _settings
 */
DifferentialCPG::DifferentialCPG(
    const ::gazebo::physics::ModelPtr &_model,
    const sdf::ElementPtr _settings,
    const std::vector< revolve::gazebo::MotorPtr > &_motors,
    const std::vector< revolve::gazebo::SensorPtr > &_sensors)
    : next_state(nullptr)
    , input(new double[_sensors.size()])
    , output(new double[_motors.size()])
{
  // Maximum iterations for init sampling/learning/no learning
  this->n_init_samples = 5;
  this->n_learning_iterations = 5;
  this->n_cooldown_iterations = 5;

  // Automatically construct plots
  this->run_analytics = true;

  // Bound for output signal
  this->abs_output_bound = 1.0;

  // If load brain is an empty string (which is by default) we train a new brain. Ex:
  // this->load_brain = "/home/maarten/projects/revolve-simulator/revolve/output/cpg_bo/1555264854/best_brain.txt";

  // Parameters
  this->evaluation_rate = 20.0;

  // Create transport node
  this->node_.reset(new gz::transport::Node());
  this->node_->Init();

  // Get Robot
  this->robot = _model;
  this->n_motors = _motors.size();

  auto name = _model->GetName();

  if (not _settings->HasElement("rv:brain"))
  {
    std::cerr << "No robot brain detected, this is probably an error."
              << std::endl;
    throw std::runtime_error("DifferentialCPG brain did not receive settings");
  }

  std::cout << _settings->GetDescription() << std::endl;
  // TODO: Make this more neat
  auto motor = _settings->GetElement("rv:brain")->GetElement("rv:actuators")->HasElement("rv:servomotor")
               ? _settings->GetElement("rv:brain")->GetElement("rv:actuators")->GetElement("rv:servomotor")
               : sdf::ElementPtr();
  while(motor)
  {
    if (not motor->HasAttribute("coordinates"))
    {
      std::cerr << "Missing required motor coordinates" << std::endl;
      throw std::runtime_error("Robot brain error");
    }

    // Split string and get coordinates
    auto coordinate_string = motor->GetAttribute("coordinates")->GetAsString();
    std::vector<std::string> coordinates;
    boost::split(coordinates, coordinate_string, boost::is_any_of(";"));

    // Pass coordinates
    auto coord_x = std::stoi(coordinates[0]);
    auto coord_y = std::stoi(coordinates[1]);
    std::cout << "coord_x,coord_y = " << coord_x << "," << coord_y << std::endl;
    auto motor_id = motor->GetAttribute("part_id")->GetAsString();
    this->positions[motor_id] = {coord_x, coord_y};

    // TODO: Determine optimization boundaries
    this->range_lb = 0.f;
    this->range_ub = 1;

    // Save neurons: bias/gain/state. Make sure initial states are of different sign.
    this->neurons[{coord_x, coord_y, 1}] = {0.f, 0.f, -this->init_state}; // Neuron A
    this->neurons[{coord_x, coord_y, -1}] = {0.f, 0.f, this->init_state}; // Neuron B

    // TODO: Add check for duplicate coordinates
    motor = motor->GetNextElement("rv:servomotor");
  }

  // Add connections between neighbouring neurons
  int i = 0;
  for (const auto &position : this->positions)
  {
    // Get name and x,y-coordinates of all neurons.
    auto name = position.first;
    int x, y; std::tie(x, y) = position.second;

    // Continue to next iteration in case there is already a connection between the 1 and -1 neuron.
    // These checks feel a bit redundant.
    // if A->B connection exists.
    if (this->connections.count({x, y, 1, x, y, -1}))
    {
      continue;
    }
    // if B->A connection exists:
    if (this->connections.count({x, y, -1, x, y, 1}))
    {
      continue;
    }

    // Loop over all positions. We call it neighbours, but we still need to check if they are a neighbour.
    for (const auto &neighbour : this->positions)
    {
      // Get information of this neuron (that we call neighbour).
      int near_x, near_y; std::tie(near_x, near_y) = neighbour.second;

      // If there is a node that is a Moore neighbour, we set it to be a neighbour for their A-nodes.
      // Thus the connections list only contains connections to the A-neighbourhood, and not the
      // A->B and B->A for some node (which makes sense).
      int dist_x = std::abs(x - near_x);
      int dist_y = std::abs(y - near_y);

      // TODO: Verify for non-spiders
      if (dist_x + dist_y == 2)
      {
        if(std::get<0>(this->connections[{x, y, 1, near_x, near_y, 1}]) != 1 or
           std::get<0>(this->connections[{near_x, near_y, 1, x, y, 1}]) != 1)
        {
          std::cout << "New connection at index " << i << ": " << x << ", " << y << ", " << near_x << ", " << near_y << "\n";
          this->connections[{x, y, 1, near_x, near_y, 1}] = std::make_tuple(1, i);
          this->connections[{near_x, near_y, 1, x, y, 1}] = std::make_tuple(1, i);
          i++;
        }
      }
    }
  }

  // Create directory for output
  this->directory_name = "output/cpg_bo/";
  this->directory_name += std::to_string(time(0)) + "/";
  std::system(("mkdir -p " + this->directory_name).c_str());

  // Initialise array of neuron states for Update() method
  this->next_state = new double[this->neurons.size()];
  this->n_weights = (int)(this->connections.size()/2) + this->n_motors;

  // Check if we want to load a pre-trained brain
  if(!this->load_brain.empty())
  {
    // Get line
    std::cout << "I will load the following brain:\n";
    std::ifstream in(this->load_brain);
    std::string line;
    std::getline(in, line);

    // Get weights in line
    std::vector<std::string> weights;
    boost::split(weights, line, boost::is_any_of(","));

    // Save weights for brain
    Eigen::VectorXd loaded_brain(this->n_weights);
    for(size_t j = 0; j < this->n_weights; j++){
      loaded_brain(j) = std::stod(weights.at(j));
      std::cout << loaded_brain(j)  << ",";

    }
    std::cout << "\n";

    // Close brain
    in.close();

    // Save these weights
    this->samples.push_back(loaded_brain);

    // Set ODE matrix at initialization
    this->set_ode_matrix();

    // Go directly into cooldown phase: Note we do require that best_sample is filled. Check this
    this->current_iteration = this->n_init_samples + this->n_learning_iterations;

    // Verbose
    std::cout << "Brain has been loaded. Skipped " << this->current_iteration << " iterations to enter cooldown mode\n";

  }
  else{
    std::cout << "Don't load existing brain\n";

    // Initialize BO
    this->bo_init();

    // Set ODE matrix at initialization
    this->set_ode_matrix();
  }


  // Initiate the cpp Evaluator
  this->evaluator.reset(new Evaluator(this->evaluation_rate));
}

/**
 * Destructor
 */
DifferentialCPG::~DifferentialCPG()
{
  delete[] this->next_state;
  delete[] this->input;
  delete[] this->output;
}

/*
 * Dummy function for limbo
 */
struct DifferentialCPG::evaluation_function{
  // number of input dimension (samples.size())
  BO_PARAM(size_t, dim_in, 18);

  // number of dimensions of the fitness
  BO_PARAM(size_t, dim_out, 1);

  Eigen::VectorXd operator()(const Eigen::VectorXd &x) const {
    return limbo::tools::make_vector(0);
  };
};

void DifferentialCPG::bo_init(){
  // BO parameters
  this->range_lb = -1.f;
  this->range_ub = 1.f;
  this->init_method = "RS"; // {RS, LHS, ORT}

  // We only want to optimize the weights for now.
  std::cout << "Number of weights = connections/2 + n_motors are "
            << this->connections.size()/2
            << " + "
            << this->n_motors
            << std::endl;

  //  // Limbo BO Parameters
  //  this->alpha = 0.5; // Acqui_UCB. Default 0.5
  //  this->delta = 0.3; // Acqui GP-UCB. Default 0.1. Convergence guaranteed in (0,1)
  //  this->l = 0.2; // Kernel width. Assumes equally sized ranges over dimensions
  //  this->sigma_sq = 0.001; // Kernel variance. 0.001 recommended
  //  this->k = 4; // EXP-ARD kernel. Number of columns used to compute M.

  // Information purposes
  std::cout << "\nSample method: " << this->init_method << ". Initial "
                                                           "samples are: \n";

  // Random sampling
  if(this->init_method == "RS") {
    for (size_t i = 0; i < this->n_init_samples; i++) {
      // Working variable to hold a random number for each weight to be optimized
      Eigen::VectorXd init_sample(this->n_weights);

      // For all weights
      for (int j = 0; j < this->n_weights; j++) {
        // Generate a random number in [0, 1]. Transform later
        double f = ((double) rand() / (RAND_MAX));

        // Append f to vector
        init_sample(j) = f;
      }

      // Save vector in samples.
      this->samples.push_back(init_sample);

      for(int k = 0; k < init_sample.size(); k ++){
        std::cout << init_sample(k) << ", ";
      }
      std::cout << "\n";
    }
  }
    // Latin Hypercube Sampling
  else if(this->init_method == "LHS"){
    // Check
    if(this->n_init_samples % this->n_weights != 0)
    {
      std::cout << "Warning: Ideally the number of initial samples is a multiple of n_weights for LHS sampling \n";
    }

    // Working variable
    double my_range = 1.f/this->n_init_samples;

    // If we have n dimensions, create n such vectors that we will permute
    std::vector<std::vector<int>> all_dimensions;

    // Fill vectors
    for (int i=0; i < this->n_weights; i++){
      std::vector<int> one_dimension;

      // Prepare for vector permutation
      for (size_t j = 0; j < this->n_init_samples; j++){
        one_dimension.push_back(j);
      }

      // Vector permutation
      std::random_shuffle(one_dimension.begin(), one_dimension.end() );

      // Save permuted vector
      all_dimensions.push_back(one_dimension);
    }

    // For all samples
    for (size_t i = 0; i < this->n_init_samples; i++){
      // Initialize Eigen::VectorXd here.
      Eigen::VectorXd init_sample(this->n_weights);

      // For all dimensions
      for (int j = 0; j < this->n_weights; j++){
        // Take a LHS
        init_sample(j) = all_dimensions.at(j).at(i)*my_range + ((double) rand() / (RAND_MAX))*my_range;
      }

      // Append sample to samples
      this->samples.push_back(init_sample);

      for(int k = 0; k < init_sample.size(); k ++){
        std::cout << init_sample(k) << ", ";
      }
      std::cout << "\n";
    }
  }
  else if(this->init_method == "ORT"){
    // Set the number of blocks per dimension
    int n_blocks = (int)(log(this->n_init_samples)/log(4));

    // Working variables
    double my_range = 1.f/this->n_init_samples;

    // Todo: Implement this check
    //    if(((log(this->n_init_samples)/log(4)) % 1.0) != 0){
    //      std::cout << "Warning: Initial number of samples is no power of 4 \n";
    //    }

    // Initiate for each  dimension a vector holding a permutation of 1,...,n_init_samples
    std::vector<std::vector<int>> all_dimensions;
    for (int i = 0; i < this->n_weights; i++) {
      // Holder for one dimension
      std::vector<int> one_dimension;
      for (size_t j = 0; j < this->n_init_samples; j++) {
        one_dimension.push_back(j);
      }

      // Do permutation
      std::random_shuffle(one_dimension.begin(), one_dimension.end());

      // Save to list
      all_dimensions.push_back(one_dimension);
    }

    // Draw n_init_samples
    for (size_t i = 0; i < this->n_init_samples; i++) {
      // Initiate new sample
      Eigen::VectorXd init_sample(this->n_weights);

      // Each dimensions will have 2^n_blocks rows it can choose from
      std::vector<int> rows_in_block;
      int end = (int)(std::pow(2, n_blocks));

      // Loop over all the blocks: we don't have to pick a block randomly
      for (int j =0; j < n_blocks; j++) {
        // Generate row numbers in this block: THIS IS WRONG
        for(int k = j*end; k < (j+1)*end; k++)
        {
          rows_in_block.push_back(k);
        }
        // Take the vector that is pointing to the actual vector
        std::vector<int> *row_numbers = &all_dimensions.at(j);

        // Get set intersection
        std::vector<int> available_rows;
        std::set_intersection(
            rows_in_block.begin(),
            rows_in_block.end(),
            row_numbers->begin(),
            row_numbers->end(),
            std::back_inserter(available_rows));

        // Shuffle available_rows
        auto rng = std::default_random_engine {};
        std::shuffle(std::begin(available_rows), std::end(available_rows), rng);

        // Draw the sample
        double sample = my_range*available_rows.at(0) + ((double) rand() /
                                                         (RAND_MAX))*my_range;
        init_sample(j) = sample;

        // Remove element from the list with available row numbers
        std::vector<int>::iterator position = std::find(available_rows.begin(),
                                                        available_rows.end(),
                                                        available_rows.at(0));

        if (position != available_rows.end())
        {
          available_rows.erase(position);
        }
      }

      // Append sample to samples
      this->samples.push_back(init_sample);

      // Print sample
      for (int h = 0; h < init_sample.size(); h++){
        std::cout << init_sample(h) << ", ";
      }
      std::cout << std::endl;
    }
  }
}

void DifferentialCPG::save_fitness(){
  // Get fitness
  double fitness = this->evaluator->Fitness();

  // Save sample if it is the best seen so far
  if(fitness >this->best_fitness){
    this->best_fitness = fitness;
    this->best_sample = this->samples.back();
  }

  // Verbose
  std::cout << "Iteration number " << this->current_iteration << " has fitness " << fitness << std::endl;

  // Limbo requires fitness value to be of type Eigen::VectorXd
  Eigen::VectorXd observation = Eigen::VectorXd(1);
  observation(0) = fitness;

  // Save fitness to std::vector. This fitness corresponds to the solution of the previous iteration
  this->observations.push_back(observation);
}

void DifferentialCPG::bo_step(){
  // Holder for sample
  Eigen::VectorXd x;

  // In case we are done with the initial random sampling. Correct for
  // initial sample taken by. Statement equivalent to !(i < n_samples -1)
  if (this->current_iteration > this->n_init_samples - 2){
    // Specify bayesian optimizer
    limbo::bayes_opt::BOptimizer<Params,
                                 limbo::initfun<Init_t>,
                                 limbo::modelfun<GP_t>,
                                 limbo::acquifun<Acqui_t>> boptimizer;

    // Optimize. Pass dummy evaluation function and observations .
    boptimizer.optimize(DifferentialCPG::evaluation_function(),
                        this->samples,
                        this->observations);

    // Get new sample
    x = boptimizer.last_sample();

    // Save this x_hat_star
    this->samples.push_back(x);
  }
}

/**
 * Callback function that defines the movement of the robot
 *
 * @param _motors
 * @param _sensors
 * @param _time
 * @param _step
 */
void DifferentialCPG::Update(
    const std::vector< revolve::gazebo::MotorPtr > &_motors,
    const std::vector< revolve::gazebo::SensorPtr > &_sensors,
    const double _time,
    const double _step)
{
  // Prevent two threads from accessing the same resource at the same time
  boost::mutex::scoped_lock lock(this->networkMutex_);

  // Read sensor data and feed the neural network
  unsigned int p = 0;
  for (const auto &sensor : _sensors)
  {
    sensor->Read(this->input + p);
    p += sensor->Inputs();
  }

  // Evaluate policy on certain time limit
  if ((_time - this->start_time) > this->evaluation_rate) {
    // Update position
    this->evaluator->Update(this->robot->WorldPose());

    // If we are still learning
    if(this->current_iteration < this->n_init_samples + this->n_learning_iterations){
      // Get and save fitness
      this->save_fitness();

      // Get new sample (weights) and add sample
      this->bo_step();

      // Set new weights
      this->set_ode_matrix();

      if (this->current_iteration < this->n_init_samples){
        std::cout << "\nEvaluating initial random sample\n";
      }
      else{
        std::cout << "\nI am learning\n";
      }
    }
      // If we are finished learning but are cooling down
    else if((this->current_iteration >= (this->n_init_samples + this->n_learning_iterations))
            and (this->current_iteration < (this->n_init_samples + this->n_learning_iterations + this->n_cooldown_iterations - 1))){
      // Save fitness
      this->save_fitness();

      // Use best sample in next iteration
      this->samples.push_back(this->best_sample);

      // Verbose
      std::cout << "\nI am cooling down \n";
    }
      // Else we don't want to update anything, but save data from this run once.
    else {
      // Save fitness of last iteration
      this->save_fitness();

      // Create plots
      if(this->run_analytics) {
        // Construct plots
        this->get_analytics();
      }
      // Exit
      std::cout << "I am finished \n";
      std::exit(0);
    }

    // Evaluation policy here
    this->start_time = _time;
    this->evaluator->Reset();
    this->current_iteration += 1;
  }

  this->step(_time, this->output);

  // Send new signals to the motors
  p = 0;
  for (const auto &motor: _motors)
  {
    motor->Update(this->output + p, _step);
    p += motor->Outputs();
  }
}


/*
 * Make matrix of weights A as defined in dx/dt = Ax.
 * Element (i,j) specifies weight from neuron i to neuron j in the system of ODEs
 */
void DifferentialCPG::set_ode_matrix(){
  // Initiate new matrix
  std::vector<std::vector<double>> matrix;

  // Fill with zeroes
  for(size_t i =0; i <this->neurons.size(); i++)
  {
    // Initialize row in matrix with zeros
    std::vector< double > row;
    for (size_t j = 0; j < this->neurons.size(); j++) row.push_back(0);
    matrix.push_back(row);
  }

  // Process A<->B connections
  int index = 0;
  for(size_t i =0; i <this->neurons.size(); i++)
  {
    // Get correct index
    int c = 0;
    if (i%2== 0){
      c = i + 1;
    }
    else{
      c = i - 1;
    }

    // Add a/b connection weight: TODO: current->iteration 5 doesn't exist.
    // Over here it should've already contain a bo_step sample
    // here yet.
    index = (int)(i/2);
    auto w  = this->samples.at(this->current_iteration)(index) *
              (this->range_ub - this->range_lb) + this->range_lb;
    matrix[i][c] = w;
    matrix[c][i] = -w;
  }

  // A<->A connections
  index++;
  int k = 0;
  std::vector<std::string> connections_seen;

  for (auto const &connection : this->connections){
    // Get connection information
    int x1, y1, z1, x2, y2, z2;
    std::tie(x1, y1, z1, x2, y2, z2) = connection.first;

    // Find location of the two neurons in this->neurons list
    int l1, l2;
    int c = 0;
    for(auto const &neuron : this->neurons){
      int x, y, z;
      std::tie(x, y, z) = neuron.first;
      if (x == x1 and y == y1 and z == z1){
        l1 = c;
      }
      else if (x == x2 and y == y2 and z == z2){
        l2 = c;
      }
      // Update counter
      c++;
    }

    // Add connection to seen connections
    if(l1 > l2){
      int l1_old = l1;
      l1 = l2;
      l2 = l1_old;
    }
    std::string connection_string = std::to_string(l1) + "-" + std::to_string(l2);

    // if not in list, add to list
    if(std::find(connections_seen.begin(), connections_seen.end(), connection_string) == connections_seen.end())
    {
      connections_seen.push_back(connection_string);
    }
      // else continue to next iteration
    else{
      continue;
    }

    // Get weight
    auto w  = this->samples.at(this->current_iteration)(index + k) *
              (this->range_ub - this->range_lb) + this->range_lb;

    // Set connection in weight matrix
    matrix[l1][l2] = w;
    matrix[l2][l1] = -w;
    k++;
  }

  // Update matrix
  this->ode_matrix = matrix;

  // Set states back to original value that is close to unit circle
  int c = 0;
  for(auto const &neuron : this->neurons){
    int x, y, z;
    std::tie(x, y, z) = neuron.first;
    if(z == -1)
    {
      this->next_state[c] = this->init_state;
    }
    else
    {
      this->next_state[c] = -this->init_state;
    }
    c++;
  }
}

/**
 * Step function
 *
 * @param _time
 * @param _output
 */
void DifferentialCPG::step(
    const double _time,
    double *_output)
{
  int neuron_count = 0;
  for (const auto &neuron : this->neurons)
  {
    // Neuron.second accesses the second 3-tuple of a neuron, containing the bias/gain/state.
    double recipient_bias, recipient_gain, recipient_state;
    std::tie(recipient_bias, recipient_gain, recipient_state) = neuron.second;

    // Save for ODE
    this->next_state[neuron_count] = recipient_state;
    neuron_count++;
  }

  // Copy values from nextstate into x for ODEINT
  state_type x(this->neurons.size());
  for (size_t i = 0; i < this->neurons.size(); i++){
    x[i] = this->next_state[i];
  }

  // Stepper. The result is saved in x. Begin time t, time step dt
  double dt = (_time - this->previous_time);
  this->previous_time = _time;

  // Perform one step
  stepper.do_step(
      [this](const state_type &x, state_type &dxdt, double t)
      {
        for(size_t i = 0; i < this->neurons.size(); i++){
          dxdt[i] = 0;
          for(size_t j = 0; j < this->neurons.size(); j++)
          {
            dxdt[i] += x[j]*this->ode_matrix[j][i];
          }
        }
      },
      x,
      _time,
      dt);

  // Copy values into nextstate
  for (size_t i = 0; i < this->neurons.size(); i++){
    this->next_state[i] = x[i];
  }

  // Loop over all neurons to actually update their states. Note that this is a new outer for loop
  auto i = 0; auto j = 0;
  for (auto &neuron : this->neurons)
  {
    // Get bias gain and state for this neuron. Note that we don't take the coordinates.
    // However, they are implicit as their order did not change.
    double bias, gain, state;
    std::tie(bias, gain, state) = neuron.second;
    double x, y, z;
    std::tie(x, y, z) = neuron.first;
    neuron.second = {bias, gain, this->next_state[i]};

    // Should be one, as output should be based on +1 neurons, which are the A neurons
    if (i % 2 == 1)
    {
      // TODO: Add Milan's function here as soon as things are working a bit
      // f(a) = (w_ao*a - bias)*gain

      // Apply saturation formula
      auto x = this->next_state[i];
      this->output[j] = this->abs_output_bound*((2.0)/(1.0 + std::pow(2.718, -2.0*x/this->abs_output_bound)) -1);
      j++;
    }
    ++i;
  }

  // Write state to file
  std::ofstream state_file;
  state_file.open(this->directory_name + "states.txt", std::ios::app);
  for(size_t i = 0; i < this->neurons.size(); i++){
    state_file << this->next_state[i] << ",";
  }
  state_file << "\n";
  state_file.close();

  // Write signal to file
  std::ofstream signal_file;
  signal_file.open(this->directory_name + "signal.txt", std::ios::app);
  for(size_t i = 0; i < this->n_motors; i++){
    signal_file << this->output[i] << ",";
  }
  signal_file << "\n";
  signal_file.close();
}

/**
 * Struct that holds the parameters on which BO is called
 */
struct DifferentialCPG::Params{
  struct bayes_opt_boptimizer : public limbo::defaults::bayes_opt_boptimizer {
  };

  // depending on which internal optimizer we use, we need to import different parameters
#ifdef USE_NLOPT
  struct opt_nloptnograd : public limbo::defaults::opt_nloptnograd {
  };
#elif defined(USE_LIBCMAES)
  struct opt_cmaes : public lm::defaults::opt_cmaes {
    };
#endif

  struct kernel : public limbo::defaults::kernel {
    BO_PARAM(double, noise, 0.00000001);

    BO_PARAM(bool, optimize_noise, false);
  };

  struct bayes_opt_bobase : public limbo::defaults::bayes_opt_bobase {
    // set stats_enabled to prevent creating all the directories
    BO_PARAM(bool, stats_enabled, false);

    BO_PARAM(bool, bounded, true);
  };

  // 1 Iteration as we will perform limbo step by step
  struct stop_maxiterations : public limbo::defaults::stop_maxiterations {
    BO_PARAM(int, iterations, 1);
  };

  struct kernel_exp : public limbo::defaults::kernel_exp {
    /// @ingroup kernel_defaults
    BO_PARAM(double, sigma_sq, 0.001);
    BO_PARAM(double, l, 0.2); // the width of the kernel. Note that it assumes equally sized ranges over dimensions
  };

  struct kernel_squared_exp_ard : public limbo::defaults::kernel_squared_exp_ard {
    /// @ingroup kernel_defaults
    BO_PARAM(int, k, 4); // k number of columns used to compute M
    /// @ingroup kernel_defaults
    BO_PARAM(double, sigma_sq, 0.001); //brochu2010tutorial p.9 without sigma_sq
  };

  struct kernel_maternfivehalves : public limbo::defaults::kernel_maternfivehalves
  {
    BO_PARAM(double, sigma_sq, 0.001); //brochu2010tutorial p.9 without sigma_sq
    BO_PARAM(double, l, 0.2); //characteristic length scale
  };

  struct acqui_gpucb : public limbo::defaults::acqui_gpucb {
    //UCB(x) = \mu(x) + \kappa \sigma(x).
    BO_PARAM(double, delta, 0.1); // default delta = 0.1, delta in (0,1) convergence guaranteed
  };

  // We do Random Sampling manually to allow for incorporation in our loop
  struct init_lhs : public limbo::defaults::init_lhs{
    BO_PARAM(int, samples, 0);
  };

  struct acqui_ucb : public limbo::defaults::acqui_ucb {
    //UCB(x) = \mu(x) + \alpha \sigma(x). high alpha have high exploration
    //iterations is high, alpha can be low for high accuracy in enough iterations.
    // In contrast, the lsow iterations should have high alpha for high
    // searching in limited iterations, which guarantee to optimal.
    BO_PARAM(double, alpha, 0.5); // default alpha = 0.5
  };
};


void DifferentialCPG::get_analytics(){
  // Generate directory name
  std::string directory_name = "output/cpg_bo/";
  directory_name += std::to_string(time(0)) + "/";
  std::system(("mkdir -p " + directory_name).c_str());

  // Write parameters to file
  std::ofstream parameters_file;
  parameters_file.open(directory_name + "parameters.txt");
  parameters_file << "Dimensions: " << this->n_weights << "\n";
  // TODO
  //parameters_file << "Kernel used: " << kernel_used << "\n";
  //parameters_file << "Acqui. function used: " << acqui_used << "\n";
  //parameters_file << "Initialization method used: " << initialization_used << "\n";
  parameters_file << "UCB alpha: " << Params::acqui_ucb::alpha() << "\n";
  parameters_file << "GP-UCB delta: " << Params::acqui_gpucb::delta() << "\n";
  parameters_file << "Kernel noise: " << Params::kernel::noise() << "\n";
  parameters_file << "No. of iterations: " << Params::stop_maxiterations::iterations() << "\n";
  parameters_file << "EXP Kernel l: " << Params::kernel_exp::l() << "\n";
  parameters_file << "EXP Kernel sigma_sq: " << Params::kernel_exp::sigma_sq() << "\n";
  parameters_file << "EXP-ARD Kernel k: "<< Params::kernel_squared_exp_ard::k() << "\n";
  parameters_file << "EXP-ARD Kernel sigma_sq: "<< Params::kernel_squared_exp_ard::sigma_sq() << "\n";
  parameters_file << "MFH Kernel sigma_sq: "<< Params::kernel_maternfivehalves::sigma_sq() << "\n";
  parameters_file << "MFH Kernel l: "<< Params::kernel_maternfivehalves::l() << "\n\n";
  parameters_file.close();

  // Save data from run
  std::ofstream observations_file;
  observations_file.open(directory_name + "fitnesses.txt");
  std::ofstream samples_file;
  samples_file.open(directory_name + "samples.txt");

  // Print to files. Do separate for debugging purposes
  for(size_t i = 0; i < (this->samples.size()); i++){
    auto sample = this->samples.at(i);
    for(int j = 0; j < this->n_weights; j++){
      samples_file << sample(j) << ", ";
    }
    samples_file << "\n";
  }
  samples_file.close();

  // Print to files. Do separate for debugging purposes
  for(size_t i = 0; i < (this->observations.size()); i++){
    // When the samples are commented out, it works.
    observations_file << this->observations.at(i) << "\n";
  }
  observations_file.close();

  // Call python file to construct plots
  std::string python_plot_command = "python3 experiments/RunAnalysisBO.py "
                                    + directory_name
                                    + " "
                                    + std::to_string((int)this->n_init_samples)
                                    + " "
                                    + std::to_string((int)this->n_cooldown_iterations);
  std::system(python_plot_command.c_str());
}
