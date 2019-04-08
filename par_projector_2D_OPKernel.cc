#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/op_kernel.h"

using namespace tensorflow; // NOLINT(build/namespaces)

#define CUDA_OPERATOR_KERNEL "ParallelProjection2D"

REGISTER_OP(CUDA_OPERATOR_KERNEL)
    .Input("volume: float")
    .Attr("volume_shape: shape")
    .Attr("projection_shape: shape")
    .Attr("volume_origin : tensor")
    .Attr("detector_origin : tensor")
    .Attr("volume_spacing : tensor")
    .Attr("detector_spacing : tensor")
    .Attr("ray_vectors : tensor")
    .Output("output: float")
    .Doc(R"doc(
Computes the 2D parallel forward projection of the input based on the given ray vectors

output: A Tensor.
  output = A*x
)doc");


void Parallel_Projection2D_Kernel_Launcher(const float *volume_ptr, float *out, const float *ray_vectors, const int number_of_projections,
                                           const int volume_width, const int volume_height, const float volume_spacing_x, const float volume_spacing_y, const float volume_origin_x, const float volume_origin_y,
                                           const int detector_width, const float detector_spacing, const float detector_origin);

class ParallelProjection2DOp : public OpKernel
{
    TensorShape volume_shape;
    int volume_width, volume_height;

    TensorShape projection_shape;
    int detector_size, number_of_projections;

    float volume_origin_x, volume_origin_y;

    float detector_origin;

    float volume_spacing_x, volume_spacing_y;

    float detector_spacing;

    Eigen::Tensor<float, 2, Eigen::RowMajor> ray_vectors_;

  public:
    explicit ParallelProjection2DOp(OpKernelConstruction *context) : OpKernel(context)
    {
        //get volume shape from attributes
        OP_REQUIRES_OK(context, context->GetAttr("volume_shape", &volume_shape));
        volume_height = volume_shape.dim_size(0);
        volume_width = volume_shape.dim_size(1);
        //get detector shape from attributes
        OP_REQUIRES_OK(context, context->GetAttr("projection_shape", &projection_shape));
        number_of_projections = projection_shape.dim_size(0);
        detector_size = projection_shape.dim_size(1);
        //get volume origin from attributes
        Tensor volume_origin_tensor;
        OP_REQUIRES_OK(context, context->GetAttr("volume_origin", &volume_origin_tensor));
        auto volume_origin_eigen = volume_origin_tensor.tensor<float, 1>();
        volume_origin_y = volume_origin_eigen(0);
        volume_origin_x = volume_origin_eigen(1);
        //get detector origin from attributes
        Tensor detector_origin_tensor;
        OP_REQUIRES_OK(context, context->GetAttr("detector_origin", &detector_origin_tensor));
        auto detector_origin_eigen = detector_origin_tensor.tensor<float, 1>();
        detector_origin = detector_origin_eigen(0);

        //get volume spacing from attributes
        Tensor volume_spacing_tensor;
        OP_REQUIRES_OK(context, context->GetAttr("volume_spacing", &volume_spacing_tensor));
        auto volume_spacing_eigen = volume_spacing_tensor.tensor<float, 1>();
        volume_spacing_y = volume_spacing_eigen(0);
        volume_spacing_x = volume_spacing_eigen(1);

        //get detector origin from attributes
        Tensor detector_spacing_tensor;
        OP_REQUIRES_OK(context, context->GetAttr("detector_spacing", &detector_spacing_tensor));
        auto detector_spacing_eigen = detector_spacing_tensor.tensor<float, 1>();
        detector_spacing = detector_spacing_eigen(0);

        //get rey vectors from attributes
        Tensor ray_vectors_tensor;
        OP_REQUIRES_OK(context, context->GetAttr("ray_vectors", &ray_vectors_tensor));
        auto ray_vectors_eigen = ray_vectors_tensor.tensor<float, 2>();
        ray_vectors_ = Eigen::Tensor<float, 2, Eigen::RowMajor>(ray_vectors_eigen);
    }

    void Compute(OpKernelContext *context) override
    {
        // Grab the input tensor
        const Tensor &input_tensor = context->input(0);
        auto input = input_tensor.flat<float>();

        // Create an output tensor
        Tensor *output_tensor = nullptr;
        OP_REQUIRES_OK(context, context->allocate_output(0, projection_shape,
                                                         &output_tensor));
        auto output = output_tensor->template flat<float>();

        // Call the cuda kernel launcher
        Parallel_Projection2D_Kernel_Launcher(input.data(), output.data(), ray_vectors_.data(), number_of_projections,
                                              volume_width, volume_height, volume_spacing_x, volume_spacing_y, volume_origin_x, volume_origin_y,
                                              detector_size, detector_spacing, detector_origin);
    }
};

REGISTER_KERNEL_BUILDER(Name(CUDA_OPERATOR_KERNEL).Device(DEVICE_GPU), ParallelProjection2DOp);


/*
 * Links the parallel-beam projector layer from python to the actual kernel implementation. Implemented according to Tensorflow API.
 * PYRO-NN is developed as an Open Source project under the GNU General Public License (GPL).
*/