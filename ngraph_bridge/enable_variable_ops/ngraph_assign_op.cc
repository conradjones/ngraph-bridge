/*******************************************************************************
 * Copyright 2019-2020 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use thi0s file except in compliance with the License.
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

#include "tensorflow/core/common_runtime/dma_helper.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/resource_mgr.h"
#include "tensorflow/core/framework/tensor_types.h"
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow/core/platform/default/logging.h"

#include "ngraph/runtime/backend.hpp"

#include "ngraph_bridge/ngraph_catalog.h"
#include "ngraph_bridge/ngraph_timer.h"
#include "ngraph_bridge/ngraph_utils.h"
#include "ngraph_bridge/ngraph_var.h"

using namespace std;
namespace ng = ngraph;

namespace tensorflow {

namespace ngraph_bridge {

/* -------------------------------------------------
//
// NGraphAssignOp
//
---------------------------------------------------*/

// Computes *input[0] = input[1]
class NGraphAssignOp : public OpKernel {
 private:
  bool copy_to_tf_;
  int ng_graph_id_;
  static int s_instance_count;
  int my_instance_id{0};

  // TODO(malikshr): Do we need these attributes, exist in TF Assign ops
  // use_exclusive_lock_, validate_shape_, relax_constraints_;

 public:
  ~NGraphAssignOp() {
    NGRAPH_VLOG(4) << "~NGraphAssignOp::" << name() << endl;
    // Delete from Input Variable Shared Name Map
    string key = NGraphCatalog::CreateNodeKey(ng_graph_id_, name(), 0);
    NGraphCatalog::DeleteFromInputVariableSharedNameMap(key);
  }

  explicit NGraphAssignOp(OpKernelConstruction* context)
      : OpKernel(context), copy_to_tf_(false) {
    OP_REQUIRES_OK(context, context->GetAttr("copy_to_tf", &copy_to_tf_));
    OP_REQUIRES_OK(context, context->GetAttr("ngraph_graph_id", &ng_graph_id_));

    NGRAPH_VLOG(4) << "NGraphAssign:: Constructor called for: " << def().name()
                   << ", copy-to-tf " << PrintBool(copy_to_tf_) << " ,Graph ID "
                   << ng_graph_id_;

    OP_REQUIRES(context, IsRefType(context->input_type(0)),
                errors::InvalidArgument("lhs input needs to be a ref type"));
    my_instance_id = s_instance_count;
    s_instance_count++;
  }

  void Compute(OpKernelContext* context) override {
    std::ostringstream oss;
    oss << "NGAssign::Compute::" << name();
    NG_TRACE(oss.str(), name(), "");

    NGRAPH_VLOG(4) << "NGraphAssign:: Compute called for: " << def().name()
                   << ", copy-to-tf " << PrintBool(copy_to_tf_) << " ,Graph ID "
                   << ng_graph_id_;

    bool log_copies = false;
    OP_REQUIRES_OK(context,
                   IsNgraphTFLogTensorCopiesEnabled(ng_graph_id_, log_copies));
    std::stringstream copy_log_str;
    copy_log_str << "KERNEL[" << type_string() << "]: " << name()
                 << " ,copy-to-tf " << PrintBool(copy_to_tf_) << "\n";
    int number_of_copies = 0;

    bool ref_exists = NGraphCatalog::ExistsInInputVariableSharedNameMap(
        ng_graph_id_, def().name(), 0);
    if (!ref_exists) {
      OP_REQUIRES(context, ref_exists,
                  errors::Internal(
                      "Caught exception : RefInput to NGAssign not found \n"));
    }
    string get_ref_var_name = NGraphCatalog::GetInputVariableSharedName(
        ng_graph_id_, def().name(), 0);

    NGraphVar* var;
    OP_REQUIRES_OK(context,
                   context->resource_manager()->Lookup<NGraphVar>(
                       context->resource_manager()->default_container(),
                       get_ref_var_name, &var));

    Tensor* rhs_tensor = (Tensor*)&(context->input(1));

    // We always return the input ref.
    context->forward_ref_input_to_ref_output(0, 0);

    // DO NOT CARE ABOUT SYNCING AS WE ARE ALWAYS SETTING THE NGTENSOR

    // Get input[1]

    // The NGraphAssign Ops with input[1] from NGraphEncap Op are removed
    // in the RemoveNGraphAssigns phase in rewrite pass
    // Assert input[1] is not from NGraphEncap Op
    // No way to get input node and check its type
    string input_1_name = def().input(1);
    OP_REQUIRES(
        context, input_1_name.find("ngraph_cluster") == -1,
        errors::Internal(
            "Caught exception: Input to NGAssign from Encapsulate Op.\n"));

    NGRAPH_VLOG(4) << "NGraphAssign:: Updating";
    if (var->update_ng_tensor(rhs_tensor)) {
      number_of_copies++;
      copy_log_str << " COPY_INP_VAL[0]";
    }

    mutex_lock l(*context->input_ref_mutex(0));
    Tensor old_lhs = context->mutable_input(0, /* lock_held */ true);

    if (copy_to_tf_) {
      if (var->copy_ng_to_tf()) {
        number_of_copies++;
        copy_log_str << " COPY_TO_TF ";
      }
    }

    copy_log_str << " Number of copies " << number_of_copies << "\n";
    if (log_copies) {
      cout << copy_log_str.str();
    }

    // Unref Var
    var->Unref();
  }
};

int NGraphAssignOp::s_instance_count = 0;

REGISTER_KERNEL_BUILDER(Name("NGraphAssign").Device(DEVICE_CPU),
                        NGraphAssignOp);

}  // namespace ngraph_bridge

}  // namespace tensorflow
