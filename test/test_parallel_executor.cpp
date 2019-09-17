/*******************************************************************************
 * Copyright 2017-2019 Intel Corporation
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
 *******************************************************************************/
#include "gtest/gtest.h"

#include <memory>

#include "tensorflow/core/common_runtime/optimization_registry.h"
#include "tensorflow/core/graph/graph_constructor.h"
#include "tensorflow/core/public/session.h"

#include "ngraph_bridge/ngraph_backend_manager.h"
#include "ngraph_bridge/ngraph_executor.h"
#include "ngraph_bridge/version.h"
#include "test/test_utilities.h"

using namespace std;
namespace ng = ngraph;
namespace tf = tensorflow;

namespace tensorflow {
namespace ngraph_bridge {
namespace testing {

Status LoadGraphFromPbTxt(const string& pb_file, const string& backend_name,
                          unique_ptr<tf::Graph>& new_graph) {
  // Read the graph
  tensorflow::GraphDef graph_def;
  auto load_graph_status = ReadTextProto(Env::Default(), pb_file, &graph_def);
  if (!load_graph_status.ok()) {
    return load_graph_status;
  }

  GraphConstructorOptions opts;
  opts.allow_internal_ops = true;
  unique_ptr<tf::Graph> input_graph =
      unique_ptr<tf::Graph>(new tf::Graph(OpRegistry::Global()));

  auto status = ConvertGraphDefToGraph(opts, graph_def, input_graph.get());
  new_graph = move(input_graph);
  return status;
}

TEST(ParallelExecutor, Construction) {
  GraphConstructorOptions opts;
  opts.allow_internal_ops = true;
  unique_ptr<tf::Graph> input_graph;

  // First test with a backend not yet created
  unique_ptr<NGraphExecutor> executor;
  ASSERT_THROW(executor = unique_ptr<NGraphExecutor>(
                   new NGraphExecutor(100, 500, 600, input_graph, "bogus")),
               std::runtime_error);

  // Next test with a null graph not yet created
  ASSERT_THROW(executor = unique_ptr<NGraphExecutor>(
                   new NGraphExecutor(100, 500, 600, input_graph, "bogus")),
               std::runtime_error);

  // Now read the graph
  ASSERT_OK(LoadGraphFromPbTxt("test_axpy_launchop.pbtxt", "INTERPRETER",
                               input_graph));

  // Next test with a backend after creating
  tf::ngraph_bridge::BackendManager::CreateBackend("INTERPRETER");
  ASSERT_NO_THROW(executor = unique_ptr<NGraphExecutor>(new NGraphExecutor(
                      100, 500, 600, input_graph, "INTERPRETER")));

  // Now that the object has been cobstructed, test various internal parts
  // TODO: Create a Test Class and mark that as a friend of the Executor class
  ASSERT_EQ(executor->GetOpBackend(), "INTERPRETER");
  ASSERT_TRUE(executor->IsTensorPipelineSupported());
}

TEST(ParallelExecutor, CompilerTest) {
  // Read the graph
  unique_ptr<tf::Graph> input_graph;

  // We are using a graph with _Arg and _Retval
  // addded i.e., a PB that is saved after the initial processing of the
  // TF graph transformation.
  ASSERT_OK(LoadGraphFromPbTxt("test_axpy_launchop.pbtxt", "INTERPRETER",
                               input_graph));

  tf::ngraph_bridge::BackendManager::CreateBackend("INTERPRETER");
  NGraphExecutor executor(100, 500, 600, input_graph, "INTERPRETER");

  // Create the inputs for this graph
  Tensor x(DT_FLOAT, TensorShape({2, 3}));
  auto x_flat = x.flat<float>();
  for (int i = 0; i < x_flat.size(); i++) {
    x_flat.data()[i] = 1.0;
  }

  Tensor y(DT_FLOAT, TensorShape({2, 3}));
  auto y_flat = y.flat<float>();
  for (int i = 0; i < y_flat.size(); i++) {
    y_flat.data()[i] = 1.0;
  }

  std::vector<Tensor> tf_input_tensors{x, y};
  vector<TensorShape> input_shapes;
  vector<const Tensor*> static_input_map;
  ng::runtime::Backend* op_backend;
  shared_ptr<ngraph::runtime::Executable> ng_exec;

  // Call the Executor to compile the funcion

  // // TODO: Investigate is the executor can decipher the static inputs
  // // from the given graph (as opposed to feeding this in externally)
  // int size = 5;
  // executor.ResizeStaticInputVector(size);

  // for (int i = 0; i < size; i++) {
  //   executor.SetStaticInputVector(i, false);
  // }

  bool cache_hit = false;
  ASSERT_OK(executor.GetNgExecutable(tf_input_tensors, input_shapes,
                                     static_input_map, op_backend, ng_exec,
                                     cache_hit));
  ASSERT_FALSE(cache_hit);

  // Now call again to test that the cache works
  ASSERT_OK(executor.GetNgExecutable(tf_input_tensors, input_shapes,
                                     static_input_map, op_backend, ng_exec,
                                     cache_hit));

  // If the cache doesn't work then the following will fire
  ASSERT_TRUE(cache_hit);

  // Now validate that the nGraph function is available
  std::shared_ptr<ngraph::Function> ng_function;
  ASSERT_EQ(executor.GetNgFunction(ng_exec, ng_function),
            tensorflow::Status::OK());

  // Validate the nGraph Function
  const auto& parameters = ng_function->get_parameters();
  ASSERT_EQ(2, parameters.size());
  cout << " Friendly name: " << ng_function->get_friendly_name()
       << " Parameters: " << parameters.size() << std::endl;
}

TEST(ParallelExecutor, PipelinedTensorCreate) {
  // Read the graph
  // We are using a graph with _Arg and _Retval
  // addded i.e., a PB that is saved after the initial processing of the
  // TF graph transformation.
  unique_ptr<tf::Graph> input_graph;
  ASSERT_OK(LoadGraphFromPbTxt("test_axpy_launchop.pbtxt", "INTERPRETER",
                               input_graph));
  tf::ngraph_bridge::BackendManager::CreateBackend("INTERPRETER");
  NGraphExecutor executor(100, 500, 600, input_graph, "INTERPRETER");

  // Create the inputs for this graph
  Tensor x(DT_FLOAT, TensorShape({2, 3}));
  auto x_flat = x.flat<float>();
  for (int i = 0; i < x_flat.size(); i++) {
    x_flat.data()[i] = 1.0;
  }

  Tensor y(DT_FLOAT, TensorShape({2, 3}));
  auto y_flat = y.flat<float>();
  for (int i = 0; i < y_flat.size(); i++) {
    y_flat.data()[i] = 1.0;
  }

  std::vector<Tensor> tf_input_tensors{x, y};
  vector<TensorShape> input_shapes;
  vector<const Tensor*> static_input_map;
  ng::runtime::Backend* op_backend;
  shared_ptr<ngraph::runtime::Executable> ng_exec;

  // Call the Executor to compile the funcion

  // TODO: Investigate is the executor can decipher the static inputs
  // from the given graph (as opposed to feeding this in externally)
  // int size = 5;
  // executor.ResizeStaticInputVector(size);

  // for (int i = 0; i < size; i++) {
  //   executor.SetStaticInputVector(i, false);
  // }

  bool cache_hit = false;
  ASSERT_OK(executor.GetNgExecutable(tf_input_tensors, input_shapes,
                                     static_input_map, op_backend, ng_exec,
                                     cache_hit));
  ASSERT_FALSE(cache_hit);
  ASSERT_EQ(2, executor.TensorPipelineDepth());

  // Get the pipelned tensors
  int pipeline_idx = -1;
  std::tuple<int, PipelinedTensorVector, PipelinedTensorVector> io_tensors;
  for (int i = 0; i < executor.TensorPipelineDepth(); i++) {
    ASSERT_OK(executor.GetTensorsFromPipeline(ng_exec, io_tensors));
    pipeline_idx = get<0>(io_tensors);
    ASSERT_EQ(i, pipeline_idx) << "GetTensorsFromPipeline() Returned: "
                               << pipeline_idx;
  }

  // Now we have exhausted all the tensors. So the next call fails
  ASSERT_NOT_OK(executor.GetTensorsFromPipeline(ng_exec, io_tensors));
}

TEST(ParallelExecutor, ExecuteOnSingleThread) {
  // Read the graph
  // We are using a graph with _Arg and _Retval
  // addded i.e., a PB that is saved after the initial processing of the
  // TF graph transformation.
  unique_ptr<tf::Graph> input_graph;
  ASSERT_OK(LoadGraphFromPbTxt("test_axpy_launchop.pbtxt", "INTERPRETER",
                               input_graph));
  tf::ngraph_bridge::BackendManager::CreateBackend("INTERPRETER");
  NGraphExecutor executor(100, 500, 600, input_graph, "INTERPRETER");

  // Create the inputs for this graph
  Tensor x(DT_FLOAT, TensorShape({2, 3}));
  Tensor y(DT_FLOAT, TensorShape({2, 3}));

  std::vector<Tensor> tf_input_tensors{x, y};
  vector<TensorShape> input_shapes;
  vector<const Tensor*> static_input_map;
  ng::runtime::Backend* op_backend;
  shared_ptr<ngraph::runtime::Executable> ng_exec;

  // Call the Executor to compile the funcion

  // TODO: Investigate is the executor can decipher the static inputs
  // from the given graph (as opposed to feeding this in externally)
  // int size = 5;
  // executor.ResizeStaticInputVector(size);
  // for (int i = 0; i < size; i++) {
  //   executor.SetStaticInputVector(i, false);
  // }

  bool cache_hit = false;
  ASSERT_OK(executor.GetNgExecutable(tf_input_tensors, input_shapes,
                                     static_input_map, op_backend, ng_exec,
                                     cache_hit));
  ASSERT_FALSE(cache_hit);

  ASSERT_EQ(2, executor.TensorPipelineDepth());

  // Get the pipelned tensors
  std::tuple<int, PipelinedTensorVector, PipelinedTensorVector> io_tensors;
  ASSERT_OK(executor.GetTensorsFromPipeline(ng_exec, io_tensors));

  // Now Fill in the tensor - X
  auto x_flat = x.flat<float>();
  for (int i = 0; i < x_flat.size(); i++) {
    x_flat.data()[i] = 1.0;
  }

  ng::element::Type ng_element_type;
  ASSERT_OK(TFDataTypeToNGraphElementType(x.dtype(), &ng_element_type));

  get<1>(io_tensors)[0]->write(
      &x_flat.data()[0], 0,
      get<1>(io_tensors)[0]->get_element_count() * ng_element_type.size());

  // Now Fill in the tensor - Y
  auto y_flat = y.flat<float>();
  for (int i = 0; i < y_flat.size(); i++) {
    y_flat.data()[i] = 1.0;
  }

  ASSERT_OK(TFDataTypeToNGraphElementType(y.dtype(), &ng_element_type));

  get<1>(io_tensors)[1]->write(
      &y_flat.data()[0], 0,
      get<1>(io_tensors)[1]->get_element_count() * ng_element_type.size());

  // Output
  vector<shared_ptr<ng::runtime::Tensor>> ng_outputs;
  for (size_t i = 0; i < ng_exec->get_results().size(); i++) {
    ng_outputs.push_back(get<2>(io_tensors)[i]);
  }

  // And execute
  ng_exec->call(ng_outputs, {get<1>(io_tensors)[0], get<1>(io_tensors)[1]});

  // Pick up the output
  vector<tf::Tensor> ngraph_outputs;
  // Convert to tf tensor
  Tensor tf_output_tensor(DT_FLOAT, TensorShape({2, 3}));
  void* dst_ptr = DMAHelper::base(&tf_output_tensor);
  ng_outputs[0]->read(dst_ptr, 0, tf_output_tensor.TotalBytes());

  cout << " NGRAPH inout X " << x.DebugString() << endl;
  cout << " NGRAPH inout Y " << y.DebugString() << endl;
  cout << " NGRAPH output " << tf_output_tensor.DebugString() << endl;

  // And validate
  // z = a * x + y
  //   a ==> 5.0
  // TODO
  Tensor expected_val(DT_FLOAT, TensorShape({2, 3}));
  AssignInputValues(expected_val, 6.0f);
  Compare(tf_output_tensor, expected_val, 0.0f);
}

TEST(ParallelExecutor, ExecuteOnMultipleThreads) {
  // Read the graph
  // We are using a graph with _Arg and _Retval
  // addded i.e., a PB that is saved after the initial processing of the
  // TF graph transformation.
  unique_ptr<tf::Graph> input_graph;
  ASSERT_OK(LoadGraphFromPbTxt("test_axpy_launchop.pbtxt", "INTERPRETER",
                               input_graph));
  tf::ngraph_bridge::BackendManager::CreateBackend("INTERPRETER");
  NGraphExecutor executor(100, 500, 600, input_graph, "INTERPRETER");

  // Create the inputs for this graph
  Tensor x(DT_FLOAT, TensorShape({2, 3}));
  Tensor y(DT_FLOAT, TensorShape({2, 3}));

  std::vector<Tensor> tf_input_tensors{x, y};
  vector<TensorShape> input_shapes;
  vector<const Tensor*> static_input_map;
  ng::runtime::Backend* op_backend;
  shared_ptr<ngraph::runtime::Executable> ng_exec;

  // Call the Executor to compile the funcion

  // TODO: Investigate is the executor can decipher the static inputs
  // from the given graph (as opposed to feeding this in externally)
  // int size = 5;
  // executor.ResizeStaticInputVector(size);

  // for (int i = 0; i < size; i++) {
  //   executor.SetStaticInputVector(i, false);
  // }

  bool cache_hit = false;
  ASSERT_OK(executor.GetNgExecutable(tf_input_tensors, input_shapes,
                                     static_input_map, op_backend, ng_exec,
                                     cache_hit));
  ASSERT_FALSE(cache_hit);
  ASSERT_EQ(2, executor.TensorPipelineDepth());

  // Now Fill in the tensor - X
  auto x_flat = x.flat<float>();
  for (int i = 0; i < x_flat.size(); i++) {
    x_flat.data()[i] = 1.0;
  }

  ng::element::Type ng_element_type;
  ASSERT_OK(TFDataTypeToNGraphElementType(x.dtype(), &ng_element_type));

  auto worker = [&](size_t thread_id) {
    // Get the pipelned tensors
    std::tuple<int, PipelinedTensorVector, PipelinedTensorVector> io_tensors;
    ASSERT_OK(executor.GetTensorsFromPipeline(ng_exec, io_tensors));

    // Copy the tensors from TensorFlow Tensor to nGraph Tensor
    // First X
    get<1>(io_tensors)[0]->write(
        &x_flat.data()[0], 0,
        get<1>(io_tensors)[0]->get_element_count() * ng_element_type.size());

    // Fill in the tensor - Y
    auto y_flat = y.flat<float>();
    for (int i = 0; i < y_flat.size(); i++) {
      y_flat.data()[i] = thread_id;
    }
    ASSERT_OK(TFDataTypeToNGraphElementType(y.dtype(), &ng_element_type));

    // Copy the tensors from TensorFlow Tensor to nGraph Tensor
    // Next Y
    get<1>(io_tensors)[1]->write(
        &y_flat.data()[0], 0,
        get<1>(io_tensors)[1]->get_element_count() * ng_element_type.size());

    // Output
    vector<shared_ptr<ng::runtime::Tensor>> ng_outputs;
    for (size_t i = 0; i < ng_exec->get_results().size(); i++) {
      ng_outputs.push_back(get<2>(io_tensors)[i]);
    }

    // And execute
    ng_exec->call(ng_outputs, {get<1>(io_tensors)[0], get<1>(io_tensors)[1]});

    // Pick up the output
    vector<tf::Tensor> ngraph_outputs;
    // Convert to tf tensor
    Tensor tf_output_tensor(DT_FLOAT, TensorShape({2, 3}));
    void* dst_ptr = DMAHelper::base(&tf_output_tensor);
    ng_outputs[0]->read(dst_ptr, 0, tf_output_tensor.TotalBytes());

    cout << " NGRAPH inout X " << x.DebugString() << endl;
    cout << " NGRAPH inout Y " << y.DebugString() << endl;
    cout << " NGRAPH output " << tf_output_tensor.DebugString() << endl;

    // And validate
    // z = a * x + y
    //   a ==> 5.0
    // TODO
    Tensor expected_val(DT_FLOAT, TensorShape({2, 3}));
    AssignInputValues(expected_val, 5.0f + thread_id);
    Compare(tf_output_tensor, expected_val, 0.0f);
  };

  std::thread thread0(worker, 0);
  std::thread thread1(worker, 1);

  thread0.join();
  thread1.join();
}

}  // namespace testing
}  // namespace ngraph_bridge
}  // namespace tensorflow