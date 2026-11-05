#pragma once
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <optional>
#include <vector>
#include "NrSubmissionLedger.h"

// Native ingress identifies SR/RR through its exact feature registry.
// Caller parameter tables are immutable; Color or Output are edited in place.
namespace DlssNr {
void EvaluateBeforeUpscale(unsigned int handle, ID3D12GraphicsCommandList* list,
    const NVSDK_NGX_Parameter* parameters, ID3D12CommandQueue* timingQueue = nullptr);
void EvaluateAfterUpscale(unsigned int handle, ID3D12GraphicsCommandList* list,
    const NVSDK_NGX_Parameter* parameters, ID3D12CommandQueue* timingQueue = nullptr);
std::vector<NrSubmissionReceipt> TakeCommandListSubmissionReceipts(
    unsigned int count, ID3D12CommandList* const* lists);
void CompleteCommandListSubmission(ID3D12CommandQueue* queue,
    const std::vector<NrSubmissionReceipt>& receipts);
void AbandonCommandListSubmission(const std::vector<NrSubmissionReceipt>& receipts);
void ReleaseForHandle(unsigned int handle);
void RetryAfterFailure();
bool IsRunning();
const char* FailureReason();
bool ModelMissing();
std::optional<double> LastGpuTime();
void RequestCapture(unsigned int frames);
bool CaptureInProgress();
void Shutdown();
}
