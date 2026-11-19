#pragma once
namespace xrfg::native_vulkan {
// Hardware fixture compiled only by XRFG_NATIVE_INGRESS_TESTS. Exercises the
// production shared-buffer transport and its exact submission/cancellation.
int run_buffer_transport_test(bool bootstrap=false,unsigned memory_trial=0);
}
