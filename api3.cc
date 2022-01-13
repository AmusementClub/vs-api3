// This file serves a an api3-to-api4 bridge: it makes it easy to write plugins
// that supports both api3 and api4 interfaces by translating api4 into api3.
//
// Only video filters are supported.
//
// Usage:
// 1. if you are using it for a plugin, just include this file in your project,
//    and write your plugin using api4. This file will provide api3 entry point
//    and automatically translate your api4 calls into equivalent api3 calls.
//
// 2. Alternatively, you can build this file in standalone mode (by defining
//    the STANDALONE macro), and then place it along side your api4 only plugin
//    libplugin.dll/.so/.dylib as libplugin.api3.dll/.so/.dylib, and then api4
//    VS will load your plugin directly and api3 VS will load this bridge dll,
//    which in turns loads your api4 plugin.
//
// Drawbacks:
// 1. It wastes some memory to provide api3/4 VideoInfo and Format conversion.
//    Defining USE_TLS macro will use thread local storage to save the cache
//    per filter instance so that the cache will not grow unbounded.
// 2. On Unix, you can't use symlink to save space for various *.api3.so bridges,
//    as VS already uses realpath to resolve symlinks when loading plugins.
#define VERSION "v1.0"
#define USE_TLS // use per-filter-instance videoinfo/format cache
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <limits.h>
#include <math.h>
#include <float.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>
#include <stdexcept>
#include <mutex>
#include <unordered_map>
#include <memory>

// Debug only
#include <iostream>
#include <stdlib.h>
static bool debug() {
	static bool d = getenv("DEBUG_V3BRIDGE") != nullptr;
	return d;
}

namespace vs4 {
#include "VapourSynth4.h"

static inline float doubleToFloatS(double d) {
	if (!isfinite(d))
		return (float)d;
	else if (d > FLT_MAX)
		return FLT_MAX;
	else if (d < -FLT_MAX)
		return -FLT_MAX;
	else
		return (float)d;
}

[[maybe_unused]] const int api_version_major = VAPOURSYNTH_API_MAJOR;
[[maybe_unused]] const int api_version_minor = VAPOURSYNTH_API_MINOR;
const int api_version = VAPOURSYNTH_API_VERSION;
#undef VAPOURSYNTH_API_MAJOR
#undef VAPOURSYNTH_API_MINOR
#undef VAPOURSYNTH_API_VERSION

// copied from vscore.cpp
static bool isValidVideoFormat(int colorFamily, int sampleType, int bitsPerSample, int subSamplingW, int subSamplingH) noexcept {
	if (colorFamily != cfUndefined && colorFamily != cfGray && colorFamily != cfYUV && colorFamily != cfRGB)
		return false;
	if (colorFamily == cfUndefined && (subSamplingH != 0 || subSamplingW != 0 || bitsPerSample != 0 || sampleType != stInteger))
		return true;
	if (sampleType != stInteger && sampleType != stFloat)
		return false;
	if (sampleType == stFloat && (bitsPerSample != 16 && bitsPerSample != 32))
		return false;
	if (subSamplingH < 0 || subSamplingW < 0 || subSamplingH > 4 || subSamplingW > 4)
		return false;
	if ((colorFamily == cfRGB || colorFamily == cfGray) && (subSamplingH != 0 || subSamplingW != 0))
		return false;
	if (bitsPerSample < 8 || bitsPerSample > 32)
		return false;
	return true;
}
static bool getVideoFormatName(const VSVideoFormat &format, char *buffer) noexcept {
	if (!isValidVideoFormat(format.colorFamily, format.sampleType, format.bitsPerSample, format.subSamplingW, format.subSamplingH))
		return false;
	char suffix[16];
	if (format.sampleType == stFloat)
		strcpy(suffix, (format.bitsPerSample == 32) ? "S" : "H");
	else
		sprintf(suffix, "%d", (format.colorFamily == cfRGB ? 3:1) * format.bitsPerSample);
	const char *yuvName = nullptr;
	switch (format.colorFamily) {
		case cfGray:
			snprintf(buffer, 32, "Gray%s", suffix);
			break;
		case cfRGB:
			snprintf(buffer, 32, "RGB%s", suffix);
			break;
		case cfYUV:
			if (format.subSamplingW == 1 && format.subSamplingH == 1)
				yuvName = "420";
			else if (format.subSamplingW == 1 && format.subSamplingH == 0)
				yuvName = "422";
			else if (format.subSamplingW == 0 && format.subSamplingH == 0)
				yuvName = "444";
			else if (format.subSamplingW == 2 && format.subSamplingH == 2)
				yuvName = "410";
			else if (format.subSamplingW == 2 && format.subSamplingH == 0)
				yuvName = "411";
			else if (format.subSamplingW == 0 && format.subSamplingH == 1)
				yuvName = "440";
			if (yuvName)
				snprintf(buffer, 32, "YUV%sP%s", yuvName, suffix);
			else
				snprintf(buffer, 32, "YUVssw%dssh%dP%s", format.subSamplingW, format.subSamplingH, suffix);
			break;
		case cfUndefined:
			snprintf(buffer, 32, "Undefined");
			break;
	}
	return true;
}
} // namespace vs4

namespace vs3 {
#define getVapourSynthAPI getVapourSynthAPI3
#include "VapourSynth.h"
#undef getVapourSynthAPI
#include "VSHelper.h"
[[maybe_unused]] const int api_version_major = VAPOURSYNTH_API_MAJOR;
[[maybe_unused]] const int api_version_minor = VAPOURSYNTH_API_MINOR;
const int api_version = VAPOURSYNTH_API_VERSION;
#undef VAPOURSYNTH_API_MAJOR
#undef VAPOURSYNTH_API_MINOR
#undef VAPOURSYNTH_API_VERSION

#ifdef _WIN32
static vs3::VSAPI *getAPI3();
#endif // _WIN32
static const vs3::VSAPI &api3 = []() -> const vs3::VSAPI &{
#ifdef _WIN32
	auto p = getAPI3();
#else // ! _WIN32
	auto p = reinterpret_cast<const vs3::VSAPI *>(vs4::getVapourSynthAPI(vs3::api_version));
#endif // _WIN32
	if (!p) {
		std::cerr << "v3bdg: unable to acquire api3 VSAPI, abort." << std::endl;
		abort();
	}
	return *p;
}();
} // namespace vs3

namespace util {
// Mostly copied from vapoursynth/src/core/vscore.cpp.
struct split1 {
	enum empties_t { empties_ok, no_empties };
};

template <typename Container>
static inline Container& split(
		Container& result,
		const typename Container::value_type& s,
		const typename Container::value_type& delimiters,
		split1::empties_t empties = split1::empties_ok) {
	result.clear();
	size_t current;
	size_t next = -1;
	do {
		if (empties == split1::no_empties) {
			next = s.find_first_not_of(delimiters, next + 1);
			if (next == Container::value_type::npos) break;
			next -= 1;
		}
		current = next + 1;
		next = s.find_first_of(delimiters, current);
		result.push_back(s.substr(current, next - current));
	} while (next != Container::value_type::npos);
	return result;
}
} // namespace util

// Type conversion
#define TYPE_PAIR(t3, t4) \
	[[maybe_unused]] static inline vs3::t3 *C(vs4::t4 *p) { return reinterpret_cast<vs3::t3 *>(p); } \
	[[maybe_unused]] static inline const vs3::t3 *C(const vs4::t4 *p) { return reinterpret_cast<const vs3::t3 *>(p); } \
	[[maybe_unused]] static inline vs4::t4 *C(vs3::t3 *p) { return reinterpret_cast<vs4::t4 *>(p); } \
	[[maybe_unused]] static inline const vs4::t4 *C(const vs3::t3 *p) { return reinterpret_cast<const vs4::t4 *>(p); }
TYPE_PAIR(VSCore, VSCore);
TYPE_PAIR(VSMap, VSMap);
TYPE_PAIR(VSFrameRef, VSFrame);
TYPE_PAIR(VSNodeRef, VSNode);
TYPE_PAIR(VSFrameContext, VSFrameContext);
TYPE_PAIR(VSFuncRef, VSFunction);
TYPE_PAIR(VSPlugin, VSPlugin);
#undef TYPE_PAIR
static inline vs3::VSColorFamily C(vs4::VSColorFamily x) {
	switch ((int)x) {
	// vs4::VSColorFamily
	case vs4::cfUndefined: return vs3::VSColorFamily(0); // no api3 equivalent
	case vs4::cfGray: return vs3::cmGray;
	case vs4::cfRGB: return vs3::cmRGB;
	case vs4::cfYUV: return vs3::cmYUV;
	// vs3::VSColorFamily
	case vs3::cmGray: return vs3::cmGray;
	case vs3::cmRGB: return vs3::cmRGB;
	case vs3::cmYUV: return vs3::cmYUV;
	default:
		std::cerr << "unsupported color family " << x << std::endl; abort();
	}
}
static inline vs4::VSColorFamily C(vs3::VSColorFamily x) {
	switch (x) {
	case vs3::cmGray: return vs4::cfGray;
	case vs3::cmRGB: return vs4::cfRGB;
	case vs3::cmYUV: return vs4::cfYUV;
	case vs3::cmYCoCg:
		std::cerr << "unsupported cmYCoCg" << std::endl; abort();
	case vs3::cmCompat:
		std::cerr << "unsupported cmCompat" << std::endl; abort();
	}
	abort(); // unreachable
}
static inline vs3::VSFilterMode C(vs4::VSFilterMode x) {
	switch (x) {
	case vs4::fmParallel: return vs3::fmParallel;
	case vs4::fmParallelRequests: return vs3::fmParallelRequests;
	case vs4::fmUnordered: return vs3::fmUnordered;
	case vs4::fmFrameState: return vs3::fmSerial;
	}
	abort(); // unreachable
}

struct pluginFunc {
	std::string name;
	vs4::VSPublicFunction func;
	vs4::VSFreeFunctionData free;
	void *data;

	pluginFunc(const std::string &name, vs4::VSPublicFunction func, vs4::VSFreeFunctionData free, void *data) :
	           name(name), func(func), free(free), data(data) {}
	~pluginFunc() {
		if (free) free(data);
	}
};

struct filterData;

static struct vs4::VSPlugin {
	vs3::VSConfigPlugin configFunc;
	vs3::VSRegisterFunction registerFunc;
	vs3::VSPlugin *plugin;
	std::string ns;
	std::vector<std::unique_ptr<pluginFunc>> funcs;
} myself;

// Format handling
static inline int copyVideoFormat(vs4::VSVideoFormat *format, const vs3::VSFormat *format3) {
	if (!format3) return 0;
	format->colorFamily = C((vs3::VSColorFamily)format3->colorFamily);
	format->sampleType = format3->sampleType;
	format->bitsPerSample = format3->bitsPerSample;
	format->bytesPerSample = format3->bytesPerSample;
	format->subSamplingW = format3->subSamplingW;
	format->subSamplingH = format3->subSamplingH;
	format->numPlanes = format3->numPlanes;
	return 1;
}
static inline int copyVideoInfo(vs4::VSVideoInfo *info, const vs3::VSVideoInfo *info3) {
	if (!info3) return 0;
	copyVideoFormat(&info->format, info3->format);
	info->fpsNum = info3->fpsNum;
	info->fpsDen = info3->fpsDen;
	info->width = info3->width;
	info->height = info3->height;
	info->numFrames = info3->numFrames;
	(void)info3->flags;
	return 1;
}
static inline int copyVideoInfo(vs3::VSVideoInfo *info, const vs4::VSVideoInfo *info4, vs4::VSCore *core) {
	if (!info4) return 0;
	const vs4::VSVideoFormat &format = info4->format;
	const vs3::VSFormat *format3 = vs3::api3.registerFormat(C((vs4::VSColorFamily)format.colorFamily), format.sampleType, format.bitsPerSample, format.subSamplingW, format.subSamplingH, C(core));
	if (!format3) return 0;
	info->format = format3;
	info->fpsNum = info4->fpsNum;
	info->fpsDen = info4->fpsDen;
	info->width = info4->width;
	info->height = info4->height;
	info->numFrames = info4->numFrames;
	info->flags = 0;
	return 1;
}

static inline void hash_combine(size_t &seed, size_t v) {
	// similar to boost::hash_combine.
	seed ^= v + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}
namespace std {
template <>
struct hash<vs3::VSVideoInfo> {
	size_t operator()(const vs3::VSVideoInfo &x) const {
		size_t seed = x.format->id;
		hash_combine(seed, x.fpsNum);
		hash_combine(seed, x.fpsDen);
		hash_combine(seed, x.width);
		hash_combine(seed, x.height);
		hash_combine(seed, x.numFrames);
		return seed;
	}
};
template <>
struct equal_to<vs3::VSVideoInfo> {
	bool operator() (const vs3::VSVideoInfo &l, const vs3::VSVideoInfo &r) const {
		return l.format->id == r.format->id && l.fpsNum == r.fpsNum && l.fpsDen == r.fpsDen && l.width == r.width &&
		       l.height == r.height && l.numFrames == r.numFrames;
	}
};
} // namespace std

// VSVideoFormat and VSVideoInfo cache
static class formatCache {
	std::mutex lock;
	std::unordered_map<int, std::unique_ptr<vs4::VSVideoFormat>> formats;
	std::unordered_map<vs3::VSVideoInfo, std::unique_ptr<vs4::VSVideoInfo>> videoInfos;
public:
	vs4::VSVideoInfo *get(const vs3::VSVideoInfo *info3) {
		if (!info3) return nullptr;
		std::lock_guard<std::mutex> g(lock);
		auto it = videoInfos.find(*info3);
		if (it != videoInfos.end())
			return it->second.get();
		std::unique_ptr<vs4::VSVideoInfo> info {new vs4::VSVideoInfo};
		auto p = info.get();
		copyVideoInfo(p, info3);
		videoInfos.emplace(*info3, std::move(info));
		return p;
	}
	vs4::VSVideoFormat *get(const vs3::VSFormat *format3) {
		std::lock_guard<std::mutex> g(lock);
		auto it = formats.find(format3->id);
		if (it != formats.end())
			return it->second.get();
		std::unique_ptr<vs4::VSVideoFormat> format {new vs4::VSVideoFormat};
		auto p = format.get();
		copyVideoFormat(p, format3);
		formats.emplace(format3->id, std::move(format));
		return p;
	}
} globalCache;
static inline formatCache *currentCache();

namespace vs4 {
#define TODO() do { std::cerr << "unimplemented " << __func__ << std::endl; abort(); } while (0)
#define IGNORE() do { std::cerr << myself.ns << ".api3: ignored call " << __func__ << std::endl; abort(); } while (0)
#define WONTIMPL() do { std::cerr << myself.ns << " calls missing function " << __func__ << std::endl; abort(); } while (0)

/* Audio and video filter related including nodes */
static std::pair<VSNode *, filterData *> VS_CC createVideoFilter_(const char *name, const VSVideoInfo *vi, VSFilterGetFrame getFrame, VSFilterFree free, int filterMode, const VSFilterDependency *dependencies, int numDeps, void *instanceData, VSCore *core) VS_NOEXCEPT;
static VSNode *VS_CC createVideoFilter2(const char *name, const VSVideoInfo *vi, VSFilterGetFrame getFrame, VSFilterFree free, int filterMode, const VSFilterDependency *dependencies, int numDeps, void *instanceData, VSCore *core) VS_NOEXCEPT;
static void VS_CC createVideoFilter(VSMap *out, const char *name, const VSVideoInfo *vi, VSFilterGetFrame getFrame, VSFilterFree free, int filterMode, const VSFilterDependency *dependencies, int numDeps, void *instanceData, VSCore *core) VS_NOEXCEPT;
static void VS_CC createAudioFilter(VSMap *out, const char *name, const VSAudioInfo *ai, VSFilterGetFrame getFrame, VSFilterFree free, int filterMode, const VSFilterDependency *dependencies, int numDeps, void *instanceData, VSCore *core) VS_NOEXCEPT { WONTIMPL(); }
static VSNode *VS_CC createAudioFilter2(const char *name, const VSAudioInfo *ai, VSFilterGetFrame getFrame, VSFilterFree free, int filterMode, const VSFilterDependency *dependencies, int numDeps, void *instanceData, VSCore *core) VS_NOEXCEPT { WONTIMPL(); }
static int VS_CC setLinearFilter(VSNode *node) VS_NOEXCEPT { return 1; }
static void VS_CC setCacheMode(VSNode *node, int mode) VS_NOEXCEPT { IGNORE(); }
static void VS_CC setCacheOptions(VSNode *node, int fixedSize, int maxSize, int maxHistorySize) VS_NOEXCEPT { IGNORE(); }

static void VS_CC freeNode(VSNode *node) VS_NOEXCEPT { vs3::api3.freeNode(C(node)); }
static VSNode *VS_CC addNodeRef(VSNode *node) VS_NOEXCEPT { return C(vs3::api3.cloneNodeRef(C(node))); }
static int VS_CC getNodeType(VSNode *node) VS_NOEXCEPT { return vs4::mtVideo; }
static const VSVideoInfo *VS_CC getVideoInfo(VSNode *node) VS_NOEXCEPT {
	const vs3::VSVideoInfo *info3 = vs3::api3.getVideoInfo(C(node));
	return currentCache()->get(info3);
}
static const VSAudioInfo *VS_CC getAudioInfo(VSNode *node) VS_NOEXCEPT { WONTIMPL(); }

/* Frame related functions */
static VSFrame *VS_CC newVideoFrame(const VSVideoFormat *format, int width, int height, const VSFrame *propSrc, VSCore *core) VS_NOEXCEPT {
	const vs3::VSFormat *format3 = vs3::api3.registerFormat(C((VSColorFamily)format->colorFamily), format->sampleType, format->bitsPerSample, format->subSamplingW, format->subSamplingH, C(core));
	return C(vs3::api3.newVideoFrame(format3, width, height, C(propSrc), C(core)));
}
static VSFrame *VS_CC newVideoFrame2(const VSVideoFormat *format, int width, int height, const VSFrame **planeSrc, const int *planes, const VSFrame *propSrc, VSCore *core) VS_NOEXCEPT {
	const vs3::VSFormat *format3 = vs3::api3.registerFormat(C((VSColorFamily)format->colorFamily), format->sampleType, format->bitsPerSample, format->subSamplingW, format->subSamplingH, C(core));
	return C(vs3::api3.newVideoFrame2(format3, width, height, reinterpret_cast<const vs3::VSFrameRef **>(planeSrc), planes, C(propSrc), C(core)));
}
static VSFrame *VS_CC newAudioFrame(const VSAudioFormat *format, int numSamples, const VSFrame *propSrc, VSCore *core) VS_NOEXCEPT { WONTIMPL(); }
static VSFrame *VS_CC newAudioFrame2(const VSAudioFormat *format, int numSamples, const VSFrame **channelSrc, const int *channels, const VSFrame *propSrc, VSCore *core) VS_NOEXCEPT { WONTIMPL(); }
static void VS_CC freeFrame(const VSFrame *f) VS_NOEXCEPT { vs3::api3.freeFrame(C(f)); }
static const VSFrame *VS_CC addFrameRef(const VSFrame *f) VS_NOEXCEPT { return C(vs3::api3.cloneFrameRef(C(f))); }
static VSFrame *VS_CC copyFrame(const VSFrame *f, VSCore *core) VS_NOEXCEPT { return C(vs3::api3.copyFrame(C(f), C(core))); }
static const VSMap *VS_CC getFramePropertiesRO(const VSFrame *f) VS_NOEXCEPT { return C(vs3::api3.getFramePropsRO(C(f))); }
static VSMap *VS_CC getFramePropertiesRW(VSFrame *f) VS_NOEXCEPT { return C(vs3::api3.getFramePropsRW(C(f))); }

static ptrdiff_t VS_CC getStride(const VSFrame *f, int plane) VS_NOEXCEPT { return vs3::api3.getStride(C(f), plane); }
static const uint8_t *VS_CC getReadPtr(const VSFrame *f, int plane) VS_NOEXCEPT { return vs3::api3.getReadPtr(C(f), plane); }
static uint8_t *VS_CC getWritePtr(VSFrame *f, int plane) VS_NOEXCEPT { return vs3::api3.getWritePtr(C(f), plane); }

static const VSVideoFormat *VS_CC getVideoFrameFormat(const VSFrame *f) VS_NOEXCEPT {
	const vs3::VSFormat *format3 = vs3::api3.getFrameFormat(C(f));
	return currentCache()->get(format3);
}
static const VSAudioFormat *VS_CC getAudioFrameFormat(const VSFrame *f) VS_NOEXCEPT { WONTIMPL(); }
static int VS_CC getFrameType(const VSFrame *f) VS_NOEXCEPT { return mtVideo; }
static int VS_CC getFrameWidth(const VSFrame *f, int plane) VS_NOEXCEPT { return vs3::api3.getFrameWidth(C(f), plane); }
static int VS_CC getFrameHeight(const VSFrame *f, int plane) VS_NOEXCEPT { return vs3::api3.getFrameHeight(C(f), plane); }
static int VS_CC getFrameLength(const VSFrame *f) VS_NOEXCEPT { WONTIMPL(); }

/* General format functions  */
static int VS_CC getVideoFormatName(const VSVideoFormat *format, char *buffer) VS_NOEXCEPT {
	if (!format) return 0;
	return vs4::getVideoFormatName(*format, buffer);
}
static int VS_CC getAudioFormatName(const VSAudioFormat *format, char *buffer) VS_NOEXCEPT { WONTIMPL(); }
static int VS_CC queryVideoFormat(VSVideoFormat *format, int colorFamily, int sampleType, int bitsPerSample, int subSamplingW, int subSamplingH, VSCore *core) VS_NOEXCEPT {
	const vs3::VSFormat *format3 = vs3::api3.registerFormat(C((VSColorFamily)colorFamily), sampleType, bitsPerSample, subSamplingW, subSamplingH, C(core));
	return copyVideoFormat(format, format3);
}
static int VS_CC queryAudioFormat(VSAudioFormat *format, int sampleType, int bitsPerSample, uint64_t channelLayout, VSCore *core) VS_NOEXCEPT { WONTIMPL(); }
static uint32_t VS_CC queryVideoFormatID(int colorFamily, int sampleType, int bitsPerSample, int subSamplingW, int subSamplingH, VSCore *core) VS_NOEXCEPT {
	if (!isValidVideoFormat(colorFamily, sampleType, bitsPerSample, subSamplingW, subSamplingH))
		return 0;
	return ((colorFamily & 0xF) << 28) | ((sampleType & 0xF) << 24) | ((bitsPerSample & 0xFF) << 16) | ((subSamplingW & 0xFF) << 8) | ((subSamplingH & 0xFF) << 0);
}
static int VS_CC getVideoFormatByID(VSVideoFormat *format, uint32_t id, VSCore *core) VS_NOEXCEPT {
	if ((id & 0xFF000000) == 0 && (id & 0x00FFFFFF)) { // v3 format id
		const vs3::VSFormat *format3 = vs3::api3.getFormatPreset(id, C(core));
		return copyVideoFormat(format, format3);
	}
	return queryVideoFormat(format, ((id >> 28) & 0xF), ((id >> 24) & 0xF), (id >> 16) & 0xFF, (id >> 8) & 0xFF, (id >> 0) & 0xFF, core);
}

/* Frame request and filter getframe functions */
static const VSFrame *VS_CC getFrame(int n, VSNode *node, char *errorMsg, int bufSize) VS_NOEXCEPT { return C(vs3::api3.getFrame(n, C(node), errorMsg, bufSize)); }
static void VS_CC getFrameAsync(int n, VSNode *node, VSFrameDoneCallback callback, void *userData) VS_NOEXCEPT { vs3::api3.getFrameAsync(n, C(node), reinterpret_cast<vs3::VSFrameDoneCallback>(callback), userData); }
static const VSFrame *VS_CC getFrameFilter(int n, VSNode *node, VSFrameContext *frameCtx) VS_NOEXCEPT { return C(vs3::api3.getFrameFilter(n, C(node), C(frameCtx))); }
static void VS_CC requestFrameFilter(int n, VSNode *node, VSFrameContext *frameCtx) VS_NOEXCEPT { vs3::api3.requestFrameFilter(n, C(node), C(frameCtx)); }
static void VS_CC releaseFrameEarly(VSNode *node, int n, VSFrameContext *frameCtx) VS_NOEXCEPT { vs3::api3.releaseFrameEarly(C(node), n, C(frameCtx)); }
static void VS_CC cacheFrame(const VSFrame *frame, int n, VSFrameContext *frameCtx) VS_NOEXCEPT { IGNORE(); }
static void VS_CC setFilterError(const char *errorMessage, VSFrameContext *frameCtx) VS_NOEXCEPT { vs3::api3.setFilterError(errorMessage, C(frameCtx)); }

/* External functions */
static void VS_CC pubFunc(const vs3::VSMap *in, vs3::VSMap *out, void *userData, vs3::VSCore *core, const vs3::VSAPI *vsapi);
static void VS_CC pubFreeFunc(void *userData) {
	pluginFunc *pd = reinterpret_cast<pluginFunc *>(userData);
	delete pd;
}

static VSFunction *VS_CC createFunction(VSPublicFunction func, void *userData, VSFreeFunctionData free, VSCore *core) VS_NOEXCEPT {
	auto *p = new pluginFunc("runtimeCreated", func, free, userData);
	return C(vs3::api3.createFunc(pubFunc, (void *)p, pubFreeFunc, C(core), &vs3::api3));
}
static void VS_CC freeFunction(VSFunction *f) VS_NOEXCEPT { vs3::api3.freeFunc(C(f)); }
static VSFunction *VS_CC addFunctionRef(VSFunction *f) VS_NOEXCEPT { return C(vs3::api3.cloneFuncRef(C(f))); }
static void VS_CC callFunction(VSFunction *func, const VSMap *in, VSMap *out) VS_NOEXCEPT { vs3::api3.callFunc(C(func), C(in), C(out), nullptr, nullptr); }

/* Map and property access functions */
static VSMap *VS_CC createMap(void) VS_NOEXCEPT { return C(vs3::api3.createMap()); }
static void VS_CC freeMap(VSMap *map) VS_NOEXCEPT { vs3::api3.freeMap(C(map)); }
static void VS_CC clearMap(VSMap *map) VS_NOEXCEPT { vs3::api3.clearMap(C(map)); }
static void VS_CC mapSetError(VSMap *map, const char *errorMessage) VS_NOEXCEPT { vs3::api3.setError(C(map), errorMessage); }
static const char *VS_CC mapGetError(const VSMap *map) VS_NOEXCEPT { return vs3::api3.getError(C(map)); }

static int VS_CC mapNumKeys(const VSMap *map) VS_NOEXCEPT { return vs3::api3.propNumKeys(C(map)); }
static const char *VS_CC mapGetKey(const VSMap *map, int index) VS_NOEXCEPT { return vs3::api3.propGetKey(C(map), index); }
static int VS_CC mapDeleteKey(VSMap *map, const char *key) VS_NOEXCEPT { return vs3::api3.propDeleteKey(C(map), key); }
static int VS_CC mapNumElements(const VSMap *map, const char *key) VS_NOEXCEPT { return vs3::api3.propNumElements(C(map), key); }
static int VS_CC mapGetType(const VSMap *map, const char *key) VS_NOEXCEPT {
	int x = vs3::api3.propGetType(C(map), key);
	switch (x) {
	case vs3::ptUnset: return vs4::ptUnset;
	case vs3::ptInt:   return vs4::ptInt;
	case vs3::ptFloat: return vs4::ptFloat;
	case vs3::ptData:  return vs4::ptData;
	case vs3::ptNode:  return vs4::ptVideoNode;
	case vs3::ptFrame: return vs4::ptVideoFrame;
	case vs3::ptFunction: return vs4::ptFunction;
	default:
		std::cerr << "mapGetType: unknown map type " << x << " for key " << key << std::endl;
		abort();
	}
}
static int VS_CC mapSetEmpty(VSMap *map, const char *key, int type) VS_NOEXCEPT {
	if (mapGetType(map, key) != vs4::ptUnset) return 1;
	switch ((VSPropertyType)type) {
	case vs4::ptInt: vs3::api3.propSetInt(C(map), key, 0, vs3::paTouch); break;
	case vs4::ptFloat: vs3::api3.propSetFloat(C(map), key, 0, vs3::paTouch); break;
	case vs4::ptData: vs3::api3.propSetData(C(map), key, nullptr, 0, vs3::paTouch); break;
	case vs4::ptVideoNode: vs3::api3.propSetNode(C(map), key, nullptr, vs3::paTouch); break;
	case vs4::ptVideoFrame: vs3::api3.propSetFrame(C(map), key, nullptr, vs3::paTouch); break;
	case vs4::ptFunction: vs3::api3.propSetFunc(C(map), key, nullptr, vs3::paTouch); break;
	default:
		std::cerr << "mapSetEmpty: unknown map v4type " << type << " for key " << key << std::endl;
		abort();
	}
	return 0;
}

#define MAP_GET2(expr, func) do { \
	if (error && mapGetError(map) != nullptr) { \
		*error = peError; \
		return 0; \
	} \
	decltype(vs3::api3.expr) r = (vs3::api3.expr); \
	if (error) { \
		switch (*error) { \
		case 0: *error = peSuccess; break; \
		case peUnset: *error = peUnset; break; \
		case peType: *error = peType; break; \
		case peIndex: *error = peIndex; break; \
		} \
	} \
	return func(r); \
} while (0)
#define MAP_GET(expr) MAP_GET2(expr,)

#define MAP_SET2(expr, post) do { \
	switch ((VSMapAppendMode)append) { \
	case maAppend: append = vs3::paAppend; break; \
	case maReplace: append = vs3::paReplace; break; \
	} \
	decltype(vs3::api3.expr) r = vs3::api3.expr; \
	post; \
	return r; \
} while(0)
#define MAP_SET(expr) MAP_SET2(expr,)

static int64_t VS_CC mapGetInt(const VSMap *map, const char *key, int index, int *error) VS_NOEXCEPT { MAP_GET(propGetInt(C(map), key, index, error)); }
static int VS_CC mapGetIntSaturated(const VSMap *map, const char *key, int index, int *error) VS_NOEXCEPT { MAP_GET2(propGetInt(C(map), key, index, error), vs3::int64ToIntS); }
static const int64_t *VS_CC mapGetIntArray(const VSMap *map, const char *key, int *error) VS_NOEXCEPT { MAP_GET(propGetIntArray(C(map), key, error)); }
static int VS_CC mapSetInt(VSMap *map, const char *key, int64_t i, int append) VS_NOEXCEPT { MAP_SET(propSetInt(C(map), key, i, append)); }
static int VS_CC mapSetIntArray(VSMap *map, const char *key, const int64_t *i, int size) VS_NOEXCEPT { return vs3::api3.propSetIntArray(C(map), key, i, size); }

static double VS_CC mapGetFloat(const VSMap *map, const char *key, int index, int *error) VS_NOEXCEPT { MAP_GET(propGetFloat(C(map), key, index, error)); }
static float VS_CC mapGetFloatSaturated(const VSMap *map, const char *key, int index, int *error) VS_NOEXCEPT { MAP_GET2(propGetFloat(C(map), key, index, error), vs4::doubleToFloatS); }
static const double *VS_CC mapGetFloatArray(const VSMap *map, const char *key, int *error) VS_NOEXCEPT { MAP_GET(propGetFloatArray(C(map), key, error)); }
static int VS_CC mapSetFloat(VSMap *map, const char *key, double d, int append) VS_NOEXCEPT { MAP_SET(propSetFloat(C(map), key, d, append)); }
static int VS_CC mapSetFloatArray(VSMap *map, const char *key, const double *d, int size) VS_NOEXCEPT { return vs3::api3.propSetFloatArray(C(map), key, d, size); }

static const char *VS_CC mapGetData(const VSMap *map, const char *key, int index, int *error) VS_NOEXCEPT { MAP_GET(propGetData(C(map), key, index, error)); }
static int VS_CC mapGetDataSize(const VSMap *map, const char *key, int index, int *error) VS_NOEXCEPT { MAP_GET(propGetDataSize(C(map), key, index, error)); }
static int VS_CC mapGetDataTypeHint(const VSMap *map, const char *key, int index, int *error) VS_NOEXCEPT { return dtUnknown; }
static int VS_CC mapSetData(VSMap *map, const char *key, const char *data, int size, int type, int append) VS_NOEXCEPT { MAP_SET(propSetData(C(map), key, data, size, append)); }

static VSNode *VS_CC mapGetNode(const VSMap *map, const char *key, int index, int *error) VS_NOEXCEPT { MAP_GET2(propGetNode(C(map), key, index, error), C); }
static int VS_CC mapSetNode(VSMap *map, const char *key, VSNode *node, int append) VS_NOEXCEPT { MAP_SET(propSetNode(C(map), key, C(node), append)); }
static int VS_CC mapConsumeNode(VSMap *map, const char *key, VSNode *node, int append) VS_NOEXCEPT { MAP_SET2(propSetNode(C(map), key, C(node), append), freeNode(node)); }

static const VSFrame *VS_CC mapGetFrame(const VSMap *map, const char *key, int index, int *error) VS_NOEXCEPT { MAP_GET2(propGetFrame(C(map), key, index, error), C); }
static int VS_CC mapSetFrame(VSMap *map, const char *key, const VSFrame *f, int append) VS_NOEXCEPT { MAP_SET(propSetFrame(C(map), key, C(f), append)); }
static int VS_CC mapConsumeFrame(VSMap *map, const char *key, const VSFrame *f, int append) VS_NOEXCEPT { MAP_SET2(propSetFrame(C(map), key, C(f), append), freeFrame(f)); }

static VSFunction *VS_CC mapGetFunction(const VSMap *map, const char *key, int index, int *error) VS_NOEXCEPT { MAP_GET2(propGetFunc(C(map), key, index, error), C); }
static int VS_CC mapSetFunction(VSMap *map, const char *key, VSFunction *func, int append) VS_NOEXCEPT { MAP_SET(propSetFunc(C(map), key, C(func), append)); }
static int VS_CC mapConsumeFunction(VSMap *map, const char *key, VSFunction *func, int append) VS_NOEXCEPT { MAP_SET2(propSetFunc(C(map), key, C(func), append), freeFunction(func)); }

static void VS_CC copyMap(const VSMap *src, VSMap *dst) VS_NOEXCEPT {
	int nkeys = mapNumKeys(src);
	for (int i = 0; i < nkeys; i++) {
		const char *key = mapGetKey(src, i);
		int t = mapGetType(src, key);
		switch (t) {
		case vs4::ptInt:
			mapSetIntArray(dst, key, mapGetIntArray(src, key, nullptr), mapNumElements(src, key)); break;
		case vs4::ptFloat:
			mapSetFloatArray(dst, key, mapGetFloatArray(src, key, nullptr), mapNumElements(src, key)); break;
		case vs4::ptData: {
			mapDeleteKey(dst, key);
			int n = mapNumElements(src, key);
			for (int i = 0; i < n; i++)
				mapSetData(dst, key, mapGetData(src, key, i, nullptr), mapGetDataSize(src, key, i, nullptr), dtUnknown, maAppend);
			break;
		}
		case vs4::ptVideoNode: {
			mapDeleteKey(dst, key);
			int n = mapNumElements(src, key);
			for (int i = 0; i < n; i++)
				mapConsumeNode(dst, key, mapGetNode(src, key, i, nullptr), maAppend);
			break;
		}
		case vs4::ptVideoFrame: {
			mapDeleteKey(dst, key);
			int n = mapNumElements(src, key);
			for (int i = 0; i < n; i++)
				mapConsumeFrame(dst, key, mapGetFrame(src, key, i, nullptr), maAppend);
			break;
		}
		case vs4::ptFunction: {
			mapDeleteKey(dst, key);
			int n = mapNumElements(src, key);
			for (int i = 0; i < n; i++)
				mapConsumeFunction(dst, key, mapGetFunction(src, key, i, nullptr), maAppend);
			break;
		}
		default:
			std::cerr << "copyMap: unknown map type " << t << " for key " << key << std::endl;
			abort();
		}
	}
}

#undef MAP_SET
#undef MAP_SET2
#undef MAP_GET
#undef MAP_GET2

/* Plugin and plugin function related */
static int VS_CC registerFunction(const char *name, const char *args, const char *returnType, VSPublicFunction argsFunc, void *functionData, VSPlugin *plugin) VS_NOEXCEPT;

static VSPlugin *VS_CC getPluginByID(const char *identifier, VSCore *core) VS_NOEXCEPT { return C(vs3::api3.getPluginById(identifier, C(core))); }
static VSPlugin *VS_CC getPluginByNamespace(const char *ns, VSCore *core) VS_NOEXCEPT { return C(vs3::api3.getPluginByNs(ns, C(core))); }
static VSPlugin *VS_CC getNextPlugin(VSPlugin *plugin, VSCore *core) VS_NOEXCEPT { TODO(); }
static const char *VS_CC getPluginName(VSPlugin *plugin) VS_NOEXCEPT { TODO(); }
static const char *VS_CC getPluginID(VSPlugin *plugin) VS_NOEXCEPT { TODO(); }
static const char *VS_CC getPluginNamespace(VSPlugin *plugin) VS_NOEXCEPT { TODO(); }
static VSPluginFunction *VS_CC getNextPluginFunction(VSPluginFunction *func, VSPlugin *plugin) VS_NOEXCEPT { TODO(); }
static VSPluginFunction *VS_CC getPluginFunctionByName(const char *name, VSPlugin *plugin) VS_NOEXCEPT { TODO(); }
static const char *VS_CC getPluginFunctionName(VSPluginFunction *func) VS_NOEXCEPT { TODO(); }
static const char *VS_CC getPluginFunctionArguments(VSPluginFunction *func) VS_NOEXCEPT { TODO(); }
static const char *VS_CC getPluginFunctionReturnType(VSPluginFunction *func) VS_NOEXCEPT { TODO(); }
static const char *VS_CC getPluginPath(const VSPlugin *plugin) VS_NOEXCEPT {
	return vs3::api3.getPluginPath(C(plugin));
}
static int VS_CC getPluginVersion(const VSPlugin *plugin) VS_NOEXCEPT { return 1; }
static VSMap *VS_CC invoke(VSPlugin *plugin, const char *name, const VSMap *args) VS_NOEXCEPT {
	if (strcmp(name, "ShufflePlanes") == 0) {
		int err;
		vs4::VSColorFamily cf = (vs4::VSColorFamily)vs3::api3.propGetInt(C(args), "colorfamily", 0, &err);
		if (err == 0)
			vs3::api3.propSetInt((vs3::VSMap *)C(args), "colorfamily", (int)C(cf), vs3::paReplace);
	}
	return C(vs3::api3.invoke(C(plugin), name, C(args)));
}

/* Core and information */
static VSCore *VS_CC createCore(int flags) VS_NOEXCEPT { WONTIMPL(); }
static void VS_CC freeCore(VSCore *core) VS_NOEXCEPT { WONTIMPL(); }
static int64_t VS_CC setMaxCacheSize(int64_t bytes, VSCore *core) VS_NOEXCEPT { return vs3::api3.setMaxCacheSize(bytes, C(core)); }
static int VS_CC setThreadCount(int threads, VSCore *core) VS_NOEXCEPT { return vs3::api3.setThreadCount(threads, C(core)); }
static void VS_CC getCoreInfo(VSCore *core, VSCoreInfo *info) VS_NOEXCEPT {
	vs3::VSCoreInfo info3;
	vs3::api3.getCoreInfo2(C(core), &info3);
	info->versionString = info3.versionString;
	info->core = info3.core;
	info->api = info3.api;
	info->maxFramebufferSize = info3.maxFramebufferSize;
	info->usedFramebufferSize = info3.usedFramebufferSize;
}
static int VS_CC getAPIVersion(void) VS_NOEXCEPT { return vs4::api_version; }

/* Message handler */
static void VS_CC logMessage(int msgType, const char *msg, VSCore *core) VS_NOEXCEPT {
	switch ((VSMessageType)msgType) {
	case mtDebug: msgType = vs3::mtDebug; break;
	case mtInformation: msgType = vs3::mtDebug; break; // no api3 equivalent
	case mtWarning: msgType = vs3::mtWarning; break;
	case mtCritical: msgType = vs3::mtCritical; break;
	case mtFatal: msgType = vs3::mtFatal; break;
	}
	vs3::api3.logMessage(msgType, msg); // no support for pre-core logging
}

static VSLogHandle *VS_CC addLogHandler(VSLogHandler handler, VSLogHandlerFree free, void *userData, VSCore *core) VS_NOEXCEPT {
	return reinterpret_cast<VSLogHandle *>((uintptr_t)vs3::api3.addMessageHandler(
	                                         reinterpret_cast<vs3::VSMessageHandler>(handler),
	                                         reinterpret_cast<vs3::VSMessageHandlerFree>(free), userData)); // no support for per-core logging
}
static int VS_CC removeLogHandler(VSLogHandle *handle, VSCore *core) VS_NOEXCEPT {
	return vs3::api3.removeMessageHandler((int)reinterpret_cast<uintptr_t>(handle));
}

static const VSAPI api4 = {
	&createVideoFilter,
	&createVideoFilter2,
	&createAudioFilter,
	&createAudioFilter2,
	&setLinearFilter,
	&setCacheMode,
	&setCacheOptions,
	&freeNode,
	&addNodeRef,
	&getNodeType,
	&getVideoInfo,
	&getAudioInfo,
	&newVideoFrame,
	&newVideoFrame2,
	&newAudioFrame,
	&newAudioFrame2,
	&freeFrame,
	&addFrameRef,
	&copyFrame,
	&getFramePropertiesRO,
	&getFramePropertiesRW,
	&getStride,
	&getReadPtr,
	&getWritePtr,
	&getVideoFrameFormat,
	&getAudioFrameFormat,
	&getFrameType,
	&getFrameWidth,
	&getFrameHeight,
	&getFrameLength,
	&getVideoFormatName,
	&getAudioFormatName,
	&queryVideoFormat,
	&queryAudioFormat,
	&queryVideoFormatID,
	&getVideoFormatByID,
	&getFrame,
	&getFrameAsync,
	&getFrameFilter,
	&requestFrameFilter,
	&releaseFrameEarly,
	&cacheFrame,
	&setFilterError,
	&createFunction,
	&freeFunction,
	&addFunctionRef,
	&callFunction,
	&createMap,
	&freeMap,
	&clearMap,
	&copyMap,
	&mapSetError,
	&mapGetError,
	&mapNumKeys,
	&mapGetKey,
	&mapDeleteKey,
	&mapNumElements,
	&mapGetType,
	&mapSetEmpty,
	&mapGetInt,
	&mapGetIntSaturated,
	&mapGetIntArray,
	&mapSetInt,
	&mapSetIntArray,
	&mapGetFloat,
	&mapGetFloatSaturated,
	&mapGetFloatArray,
	&mapSetFloat,
	&mapSetFloatArray,
	&mapGetData,
	&mapGetDataSize,
	&mapGetDataTypeHint,
	&mapSetData,
	&mapGetNode,
	&mapSetNode,
	&mapConsumeNode,
	&mapGetFrame,
	&mapSetFrame,
	&mapConsumeFrame,
	&mapGetFunction,
	&mapSetFunction,
	&mapConsumeFunction,
	&registerFunction,
	&getPluginByID,
	&getPluginByNamespace,
	&getNextPlugin,
	&getPluginName,
	&getPluginID,
	&getPluginNamespace,
	&getNextPluginFunction,
	&getPluginFunctionByName,
	&getPluginFunctionName,
	&getPluginFunctionArguments,
	&getPluginFunctionReturnType,
	&getPluginPath,
	&getPluginVersion,
	&invoke,
	&createCore,
	&freeCore,
	&setMaxCacheSize,
	&setThreadCount,
	&getCoreInfo,
	&getAPIVersion,
	&logMessage,
	&addLogHandler,
	&removeLogHandler,
};

#undef WONTIMPL
#undef IGNORE
#undef TODO
} // namespace vs4

// Filter Wrapper
struct filterData {
	vs4::VSFilterGetFrame getFrameFunc;
	vs4::VSFilterFree freeFunc;
	void *data;
	vs3::VSMap *tmpMap;
	vs3::VSVideoInfo info3;
	formatCache cache;

	filterData(vs4::VSFilterGetFrame getFrameFunc, vs4::VSFilterFree freeFunc, void *data):
	           getFrameFunc(getFrameFunc), freeFunc(freeFunc), data(data) {
		tmpMap = vs3::api3.createMap();
	}
	~filterData() {
		vs3::api3.freeMap(tmpMap);
	}
};
#ifdef USE_TLS
static thread_local filterData *current;
class setCurrent {
	filterData *old;
public:
	setCurrent(filterData *d) { old = current; current = d; }
	~setCurrent() { current = old; }
};
#else
struct setCurrent {
	setCurrent(filterData *d) {}
};
#endif

static inline formatCache *currentCache() {
#ifdef USE_TLS
	if (current) return &current->cache;
#endif
	return &globalCache;
}

static void VS_CC initFunc3(vs3::VSMap *in, vs3::VSMap *out, void **instanceData, vs3::VSNode *node, vs3::VSCore *core, const vs3::VSAPI *vsapi) {
	filterData *fd = reinterpret_cast<filterData *>(*instanceData);
	vs3::api3.setVideoInfo(&fd->info3, 1, node);
}
static const vs3::VSFrameRef * VS_CC getFrameFunc3(int n, int activationReason, void **instanceData, void **frameData, vs3::VSFrameContext *frameCtx, vs3::VSCore *core, const vs3::VSAPI *vsapi) {
	filterData *fd = reinterpret_cast<filterData *>(*instanceData);
	setCurrent _(fd);
	void *ctx[4] = { nullptr, nullptr, nullptr, nullptr };
	void **fdata = *frameData ? reinterpret_cast<void **>(*frameData) : &ctx[0];
	switch (activationReason) {
	case vs3::arFrameReady: return nullptr;
	case vs3::arError: {
		auto r = fd->getFrameFunc(n, vs4::arError, fd->data, fdata, C(frameCtx), C(core), &vs4::api4);
		if (*frameData) {
			free(*frameData);
			*frameData = nullptr;
		}
		return C(r);
	}
	case vs3::arInitial: {
		auto r = fd->getFrameFunc(n, vs4::arInitial, fd->data, fdata, C(frameCtx), C(core), &vs4::api4);
		if (fdata[0]) {
			*frameData = malloc(4 * sizeof(void *));
			memcpy(*frameData, fdata, 4 * sizeof(void *));
		}
		return C(r);
	}
	case vs3::arAllFramesReady: {
		auto r = fd->getFrameFunc(n, vs4::arAllFramesReady, fd->data, fdata, C(frameCtx), C(core), &vs4::api4);
		if (r && *frameData) {
			free(*frameData);
			*frameData = nullptr;
		}
		return C(r);
	}
	}
	return nullptr;
}
static void VS_CC freeFunc3(void *instanceData, vs3::VSCore *core, const vs3::VSAPI *vsapi) {
	filterData *fd = reinterpret_cast<filterData *>(instanceData);
	setCurrent _(fd);
	fd->freeFunc(fd->data, C(core), &vs4::api4);
	delete fd;
}

namespace vs4 {
static std::pair<VSNode *, filterData *> VS_CC createVideoFilter_(const char *name, const VSVideoInfo *vi, VSFilterGetFrame getFrame, VSFilterFree free, int filterMode, const VSFilterDependency *dependencies, int numDeps, void *instanceData, VSCore *core) VS_NOEXCEPT {
	(void) dependencies;
	auto mode = C((vs4::VSFilterMode)filterMode);
	bool isSrcFilter = numDeps == 0 && mode == vs3::fmUnordered;
	auto fd = new filterData { getFrame, free, instanceData };
	copyVideoInfo(&fd->info3, vi, core);
	std::string name2(name);
	name2 += ".api4";
	vs3::api3.createFilter(fd->tmpMap, fd->tmpMap, name2.c_str(), initFunc3, getFrameFunc3, freeFunc3, mode,
	                        isSrcFilter ? vs3::nfMakeLinear : 0, reinterpret_cast<void *>(fd), C(core));
	const char *errMsg = vs3::api3.getError(fd->tmpMap);
	if (errMsg) {
		std::cerr << "api3.createFilter failed: " << errMsg << std::endl;
		delete fd;
		return {nullptr, nullptr};
	}
	auto node = vs3::api3.propGetNode(fd->tmpMap, "clip", 0, nullptr);
	vs3::api3.propDeleteKey(fd->tmpMap, "clip");
	return {C(node), fd};
}
static VSNode *VS_CC createVideoFilter2(const char *name, const VSVideoInfo *vi, VSFilterGetFrame getFrame, VSFilterFree free, int filterMode, const VSFilterDependency *dependencies, int numDeps, void *instanceData, VSCore *core) VS_NOEXCEPT {
	auto p = createVideoFilter_(name, vi, getFrame, free, filterMode, dependencies, numDeps, instanceData, core);
	return p.first;
}
static void VS_CC createVideoFilter(VSMap *out, const char *name, const VSVideoInfo *vi, VSFilterGetFrame getFrame, VSFilterFree free, int filterMode, const VSFilterDependency *dependencies, int numDeps, void *instanceData, VSCore *core) VS_NOEXCEPT {
	auto p = createVideoFilter_(name, vi, getFrame, free, filterMode, dependencies, numDeps, instanceData, core);
	if (!p.first) {
		vs4::api4.mapSetError(out, vs3::api3.getError(p.second->tmpMap));
		return;
	}
	vs4::api4.mapConsumeNode(out, "clip", p.first, vs4::maAppend);
}
} // namespace vs4

// Plugin API
static int VS_CC getAPIVersion(void) VS_NOEXCEPT { return vs4::api_version; }
static int VS_CC configPlugin(const char *identifier, const char *pluginNamespace, const char *name, int pluginVersion, int apiVersion, int flags, vs4::VSPlugin *plugin) VS_NOEXCEPT {
	plugin->configFunc(identifier, pluginNamespace, name, vs3::api_version, flags == 0, plugin->plugin);
	plugin->ns = pluginNamespace;
	return 1;
}
static void VS_CC vs4::pubFunc(const vs3::VSMap *in, vs3::VSMap *out, void *userData, vs3::VSCore *core, const vs3::VSAPI *vsapi) {
	pluginFunc *pd = reinterpret_cast<pluginFunc *>(userData);
	pd->func(C(in), C(out), pd->data, C(core), &vs4::api4);
}

static int VS_CC vs4::registerFunction(const char *name, const char *args, const char *returnType, vs4::VSPublicFunction argsFunc, void *functionData, vs4::VSPlugin *plugin) VS_NOEXCEPT {
	std::vector<std::string> argList, argsOut;
	util::split(argList, std::string(args), std::string(";"), util::split1::no_empties);
	std::string args3;
	for (const std::string &arg : argList) {
		std::vector<std::string> argParts;
		util::split(argParts, arg, std::string(":"), util::split1::no_empties);

		if (argParts.size() == 1 && argParts[0] == "any")
			return 0; // no support for "any"

		std::string &typeName = argParts[1];
		bool arr = false;
		if (typeName.length() > 2 && typeName.substr(typeName.length() - 2) == "[]") {
			typeName.resize(typeName.length() - 2);
			arr = true;
		}
		if (typeName == "anode" || typeName == "aframe")
			return 0; // no support for audio filters
		else if (typeName == "vnode") typeName = "clip";
		else if (typeName == "vframe") typeName = "frame";

		args3 += argParts[0] + ":";
		args3 += typeName;
		if (arr) args3 += "[]";
		if (argParts.size() == 3)
			args3 += ":" + argParts[2];
		args3 += ";";
	}
	if (debug()) std::cerr << "function " << plugin->ns << "." << name << ": arg4 = " << args << " -> arg3 = " << args3 << std::endl;
	std::unique_ptr<pluginFunc> pf(new pluginFunc(name, argsFunc, nullptr, functionData));
	plugin->funcs.push_back(std::move(pf));
	plugin->registerFunc(name, args3.c_str(), pubFunc, plugin->funcs.back().get(), plugin->plugin);
	return 1;
}
static vs4::VSPLUGINAPI pluginAPI4 {
	getAPIVersion,
	configPlugin,
	vs4::registerFunction,
};

static void wrap(
	vs4::VSInitPlugin init,
	vs3::VSConfigPlugin configFunc, vs3::VSRegisterFunction registerFunc, vs3::VSPlugin *plugin) {
	myself.configFunc = configFunc;
	myself.registerFunc = registerFunc;
	myself.plugin = plugin;
	init(&myself, &pluginAPI4);
}

#ifdef _WIN32
#include <windows.h>
static vs3::VSAPI *vs3::getAPI3() {
	HMODULE h = LoadLibraryW(L"VapourSynth.dll");
	if (!h) return nullptr;
	vs3::VSAPI *(VS_CC *f)(int) = (decltype(f))(void *)GetProcAddress(h, "getVapourSynthAPI");
	if (!f) f = (decltype(f))(void *)GetProcAddress(h, "_getVapourSynthAPI@4");
	return f(vs3::api_version);
}
#endif

#ifndef STANDALONE
VS_EXTERNAL_API(void) VapourSynthPluginInit2(vs4::VSPlugin *plugin, const vs4::VSPLUGINAPI *vspapi);
VS_EXTERNAL_API(void) VapourSynthPluginInit(vs3::VSConfigPlugin configFunc, vs3::VSRegisterFunction registerFunc, vs3::VSPlugin *plugin) {
	wrap(VapourSynthPluginInit2, configFunc, registerFunc, plugin);
}
#else // STANDALONE
#ifdef _WIN32
static vs4::VSInitPlugin getInit4() {
	HMODULE mod = 0;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (char *)getInit4, &mod) == 0)
		throw std::runtime_error("unable to locate myself");
	std::vector<wchar_t> buf;
	size_t n = 0;
	do {
		buf.resize(buf.size() + MAX_PATH);
		n = GetModuleFileNameW(mod, buf.data(), buf.size());
	} while (n >= buf.size());
	buf.resize(n);
	std::wstring path(buf.begin(), buf.end());
	size_t idx = path.rfind(L'.', path.size() - 5); // skip trailing ".dll"
	if (idx == std::wstring::npos) return nullptr;
	std::wstring dllPath = path.substr(0, idx) + L".dll";
	if (debug())
		std::wcerr << L"v3bdg: self = " << path << L", dll = " << dllPath << std::endl;
	HMODULE h = LoadLibraryW(dllPath.c_str());
	if (h == 0)
		return nullptr;
	void *f = (void *)GetProcAddress(h, "VapourSynthPluginInit2");
	if (!f)
		f = (void *)GetProcAddress(h, "_VapourSynthPluginInit2@8");
	if (!f)
		FreeLibrary(h);
	return reinterpret_cast<vs4::VSInitPlugin>(f);
}
#else // !_WIN32
#include <dlfcn.h> // we require dladdr
static vs4::VSInitPlugin getInit4() {
	Dl_info info;
	if (dladdr((void *)getInit4, &info) == 0)
		throw std::runtime_error("unable to locate myself");
	std::string path(info.dli_fname);
	size_t idx = path.rfind('.');
	std::string suffix = path.substr(idx);
	idx = path.rfind(L'.', idx - 1);
	if (idx == std::wstring::npos) return nullptr;
	std::string dllPath = path.substr(0, idx) + suffix;
	if (debug())
		std::cerr << "v3bdg: self = " << path << ", dll = " << dllPath << std::endl;
	void *h = dlopen(dllPath.c_str(), RTLD_GLOBAL | RTLD_LAZY);
	if (h == 0) return nullptr;
	void *f = dlsym(h, "VapourSynthPluginInit2");
	if (!f)
		dlclose(h);
	return reinterpret_cast<vs4::VSInitPlugin>(f);
}
#endif // _WIN32
VS_EXTERNAL_API(void) VapourSynthPluginInit2(vs4::VSPlugin *plugin, const vs4::VSPLUGINAPI *vspapi) {
	(void) plugin;
	(void) vspapi;
}
static void VS_CC versionCreate(const vs3::VSMap *in, vs3::VSMap *out, void *user_data, vs3::VSCore *core, const vs3::VSAPI *vsapi) {
	vsapi->propSetData(out, "version", VERSION, -1, vs3::paAppend);
}
VS_EXTERNAL_API(void) VapourSynthPluginInit(vs3::VSConfigPlugin configFunc, vs3::VSRegisterFunction registerFunc, vs3::VSPlugin *plugin) {
	vs4::VSInitPlugin init = getInit4();
	if (init)
		wrap(init, configFunc, registerFunc, plugin);
	registerFunc("api3_bridged", "", versionCreate, nullptr, plugin);
}
#endif // STANDALONE
