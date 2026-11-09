"""Prepare explicit, pinned development dependencies. No global SDK/PATH edits."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import urllib.request
import zipfile

NCNN = "00357e2f20ea2f8245276c2fa1e443e15a4f6bfa"
PNNX_SHA = "e4f91348a399a0060a6e7dd323d40427e1cbdcab67469cf1904b5d09b23606c5"
JSON_SHA = "aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63"


def download(url, path, digest):
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        partial = path.with_suffix(path.suffix + ".partial")
        urllib.request.urlretrieve(url, partial)
        if hashlib.sha256(partial.read_bytes()).hexdigest() != digest:
            raise RuntimeError("SHA256 mismatch: " + path.name)
        partial.replace(path)


def replace_once(path, before, after):
    text = path.read_text(encoding="utf-8")
    if after in text:
        return
    if text.count(before) != 1:
        raise RuntimeError("Pinned ncnn patch context changed: " + str(path))
    path.write_text(text.replace(before, after), encoding="utf-8", newline="\n")


def prepare(root):
    root.mkdir(parents=True, exist_ok=True)
    ncnn = root / "ncnn"
    if not ncnn.exists():
        subprocess.run(["git", "clone", "--no-checkout", "https://github.com/Tencent/ncnn.git", str(ncnn)], check=True)
        subprocess.run(["git", "-C", str(ncnn), "checkout", "--detach", NCNN], check=True)
    revision = subprocess.check_output(["git", "-C", str(ncnn), "rev-parse", "HEAD"], text=True).strip()
    if revision != NCNN:
        raise RuntimeError("ncnn is not the pinned revision; preserve it and select another --deps")
    subprocess.run(["git", "-C", str(ncnn), "submodule", "update", "--init", "--depth", "1", "glslang"], check=True)
    # ncnn's device is intentionally reused: no second Vulkan device or CPU
    # staging bridge. Enable the optional Win32 external-memory extension only
    # when the selected device advertises it.
    before = '    if (info.support_VK_KHR_external_memory())\n        enabledExtensions.push_back("VK_KHR_external_memory");'
    after = before + '''
#if _WIN32
    uint32_t ofxr_count = 0;
    vkEnumerateDeviceExtensionProperties(info.physical_device(), 0, &ofxr_count, 0);
    std::vector<VkExtensionProperties> ofxr_extensions(ofxr_count);
    vkEnumerateDeviceExtensionProperties(info.physical_device(), 0, &ofxr_count, ofxr_extensions.data());
    for (const auto& ext : ofxr_extensions)
        if (strcmp(ext.extensionName, "VK_KHR_external_memory_win32") == 0)
            enabledExtensions.push_back("VK_KHR_external_memory_win32");
#endif'''
    replace_once(ncnn / "src/gpu.cpp", before, after)
    path = ncnn / "src/command.h"
    text = path.read_text(encoding="utf-8")
    marker = "    void ofxr_external_barrier(const VkMat& binding, bool acquire);"
    if marker not in text:
        text = text.replace("    int submit_and_wait();", marker + "\n\n    int submit_and_wait();", 1)
        path.write_text(text, encoding="utf-8", newline="\n")
    method = '''
void VkCompute::ofxr_external_barrier(const VkMat& binding, bool acquire)
{
    VkBufferMemoryBarrier* barriers = new VkBufferMemoryBarrier[1]();
    barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barriers[0].srcAccessMask = acquire ? 0 : binding.data->access_flags;
    barriers[0].dstAccessMask = acquire ? VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT : 0;
    barriers[0].srcQueueFamilyIndex = acquire ? ~1u : vkdev->info.compute_queue_family_index();
    barriers[0].dstQueueFamilyIndex = acquire ? vkdev->info.compute_queue_family_index() : ~1u;
    barriers[0].buffer = binding.buffer();
    barriers[0].offset = binding.data->offset;
    barriers[0].size = binding.data->capacity;
    const VkPipelineStageFlags src = acquire ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : binding.data->stage_flags;
    const VkPipelineStageFlags dst = acquire ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    if (vkdev->info.support_VK_KHR_push_descriptor())
    {
        vkCmdPipelineBarrier(d->compute_command_buffer, src, dst, 0, 0, 0, 1, barriers, 0, 0);
        delete[] barriers;
    }
    else
    {
        VkComputePrivate::record r;
        r.type = VkComputePrivate::record::TYPE_buffer_barrers;
        r.command_buffer = d->compute_command_buffer;
        r.buffer_barrers.src_stage = src;
        r.buffer_barrers.dst_stage = dst;
        r.buffer_barrers.barrier_count = 1;
        r.buffer_barrers.barriers = barriers;
        d->delayed_records.push_back(r);
    }
    binding.data->access_flags = acquire ? VK_ACCESS_MEMORY_WRITE_BIT : 0;
    binding.data->stage_flags = acquire ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
}
'''
    replace_once(ncnn / "src/command.cpp", "void VkCompute::barrier_readwrite(const VkMat& binding)", method + "\nvoid VkCompute::barrier_readwrite(const VkMat& binding)")
    download("https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp", root / "json.hpp", JSON_SHA)
    download("https://raw.githubusercontent.com/nlohmann/json/v3.12.0/LICENSE.MIT", root / "json-LICENSE.MIT", "46a65cffd1ea955132d95a8dd921640714a8d6b537d2e4e482d31145ae95b603")
    archive = root / "pnnx-20261020-windows.zip"
    download("https://github.com/pnnx/pnnx/releases/download/20261020/pnnx-20261020-windows.zip", archive, PNNX_SHA)
    with zipfile.ZipFile(archive) as z:
        for name in ("pnnx-20261020-windows/pnnx.exe", "pnnx-20261020-windows/README.md"):
            z.extract(name, root / "pnnx")
    print("Pinned ncnn, glslang, JSON and pnnx ready in", root)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--deps", type=Path, required=True)
    prepare(parser.parse_args().deps.resolve())
