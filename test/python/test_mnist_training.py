# ==============================================================================
#  Copyright 2019 Intel Corporation
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  you may not use this file except in compliance with the License.
#  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.
# ==============================================================================

import sys
import pytest
import getpass
import tensorflow as tf
import ngraph_bridge

import numpy as np
from common import NgraphTest

# This test needs access to mnist_deep_simplified.py script
# present in ngraph-bridge/examples/mnist
# so this path needs to be added to python path when running this

# For eg. when running the test from ngraph-bridge/build_cmake/test/python
# you can add this path as below
# sys.path.insert(0, '../../../examples/mnist')

from mnist_deep_simplified import *


class TestMnistTraining(NgraphTest):

    # TODO bring back parameterization
    #@pytest.mark.parametrize(("optimizer"), ("adam", "sgd", "momentum"))
    @pytest.mark.parametrize(("optimizer"), ("adam",))
    def test_mnist_training(self, optimizer):

        class mnist_training_flags:

            def __init__(self, data_dir, output_model_dir, training_iterations,
                         training_batch_size, validation_batch_size,
                         make_deterministic, training_optimizer,
                         input_model_dir):
                self.data_dir = data_dir
                self.output_model_dir = output_model_dir
                self.train_loop_count = training_iterations
                self.batch_size = training_batch_size
                self.test_image_count = validation_batch_size
                self.make_deterministic = make_deterministic
                self.optimizer = optimizer
                self.input_model_dir = input_model_dir

        data_dir = '/tmp/' + getpass.getuser() + 'tensorflow/mnist/input_data'
        train_loop_count = 20
        batch_size = 50
        test_image_count = None
        make_deterministic = True
        output_model_dir_ng = './save_loc_ng/'

        FLAGS = mnist_training_flags(
            data_dir, output_model_dir_ng, train_loop_count, batch_size,
            test_image_count, make_deterministic, optimizer, None)

        # Run on nGraph
        ng_loss_values, ng_test_accuracy = train_mnist_cnn(FLAGS)
        ng_values = ng_loss_values + [ng_test_accuracy]
        # Reset the Graph
        tf.reset_default_graph()

        # disable ngraph-tf
        ngraph_bridge.disable()
        output_model_dir_tf = './save_loc_tf/'
        FLAGS.output_model_dir = output_model_dir_tf
        tf_loss_values, tf_test_accuracy = train_mnist_cnn(FLAGS)
        tf_values = tf_loss_values + [tf_test_accuracy]

        # compare values
        assert np.allclose(
            ng_values, tf_values,
            atol=1e-3), "Loss or Accuracy values don't match"

        tf.reset_default_graph()

        # Now resume training from output_model_dir
        # and dump new model in output_model_dir_ng_2
        output_model_dir_ng_2 = './save_loc_ng_2/'
        FLAGS = mnist_training_flags(
            data_dir, output_model_dir_ng_2, train_loop_count, batch_size,
            test_image_count, make_deterministic, optimizer, output_model_dir_ng)
        # Run on nGraph
        ng_loss_values, ng_test_accuracy = train_mnist_cnn(FLAGS)
        ng_values = ng_loss_values + [ng_test_accuracy]
        # Reset the Graph
        tf.reset_default_graph()

        # disable ngraph-tf
        ngraph_bridge.disable()
        output_model_dir_tf = './save_loc_tf_2/'
        FLAGS.output_model_dir = output_model_dir_tf
        tf_loss_values, tf_test_accuracy = train_mnist_cnn(FLAGS)
        tf_values = tf_loss_values + [tf_test_accuracy]

        # compare values
        assert np.allclose(
            ng_values, tf_values,
            atol=1e-3), "Loss or Accuracy values don't match"

        #shutil.rmtree(save_loc_tf_2)
        #shutil.rmtree(save_loc_ng_2)
        #shutil.rmtree(save_loc_tf)
        #shutil.rmtree(save_loc_ng)
