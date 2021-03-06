/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "Sound.h"

#include <cstdlib>
#include <cmath>
#include <alc.h>

#ifndef ALC_ALL_DEVICES_SPECIFIER
#define ALC_ALL_DEVICES_SPECIFIER 0x1013
// needed for ALC_ALL_DEVICES_SPECIFIER on some special *nix
// #include <alext.h>
#endif

#include <climits>
#include <cinttypes>
#include <functional>

#include "System/Sound/ISoundChannels.h"
#include "System/Sound/SoundLog.h"
#include "SoundSource.h"
#include "SoundBuffer.h"
#include "SoundItem.h"
#include "ALShared.h"
#include "EFX.h"
#include "EFXPresets.h"

#include "System/Config/ConfigHandler.h"
#include "System/Exceptions.h"
#include "System/FileSystem/FileHandler.h"
#include "Lua/LuaParser.h"
#include "Map/Ground.h"
#include "Sim/Misc/GlobalConstants.h"
#include "System/myMath.h"
#include "System/SafeUtil.h"
#include "System/StringUtil.h"
#include "System/Platform/Threading.h"
#include "System/Platform/Watchdog.h"
#include "System/Threading/SpringThreading.h"

#include "System/float3.h"


spring::recursive_mutex soundMutex;


CSound::CSound()
	: listenerNeedsUpdate(false)
	, soundThread(nullptr)
	, soundThreadQuit(false)
	, canLoadDefs(false)
{
	std::lock_guard<spring::recursive_mutex> lck(soundMutex);
	mute = false;
	appIsIconified = false;
	pitchAdjust = configHandler->GetBool("PitchAdjust");

	masterVolume = configHandler->GetInt("snd_volmaster") * 0.01f;
	Channels::General->SetVolume(configHandler->GetInt("snd_volgeneral") * 0.01f);
	Channels::UnitReply->SetVolume(configHandler->GetInt("snd_volunitreply") * 0.01f);
	Channels::UnitReply->SetMaxConcurrent(1);
	Channels::UnitReply->SetMaxEmits(1);
	Channels::Battle->SetVolume(configHandler->GetInt("snd_volbattle") * 0.01f);
	Channels::UserInterface->SetVolume(configHandler->GetInt("snd_volui") * 0.01f);
	Channels::BGMusic->SetVolume(configHandler->GetInt("snd_volmusic") * 0.01f);

	SoundBuffer::Initialise();
	soundItems.push_back(nullptr);

	soundThread = new spring::thread();
	*soundThread = Threading::CreateNewThread(std::bind(&CSound::UpdateThread, this, configHandler->GetInt("MaxSounds")));

	configHandler->NotifyOnChange(this, {"snd_volmaster", "snd_eaxpreset", "snd_filter", "UseEFX", "snd_volgeneral", "snd_volunitreply", "snd_volbattle", "snd_volui", "snd_volmusic", "PitchAdjust"});
}

CSound::~CSound()
{
	soundThreadQuit = true;
	configHandler->RemoveObserver(this);

	LOG_L(L_INFO, "[%s][1] soundThread=%p", __FUNCTION__, soundThread);

	if (soundThread != nullptr) {
		soundThread->join();
		spring::SafeDelete(soundThread);
	}

	LOG_L(L_INFO, "[%s][2]", __FUNCTION__);

	for (SoundItem* item: soundItems)
		delete item;

	soundItems.clear();
	SoundBuffer::Deinitialise();

	LOG_L(L_INFO, "[%s][3]", __FUNCTION__);
}

bool CSound::HasSoundItem(const std::string& name) const
{
	if (soundMap.find(name) != soundMap.end())
		return true;

	return (soundItemDefsMap.find(StringToLower(name)) != soundItemDefsMap.end());
}

size_t CSound::GetSoundId(const std::string& name)
{
	std::lock_guard<spring::recursive_mutex> lck(soundMutex);

	if (soundSources.empty())
		return 0;

	auto it = soundMap.find(name);
	if (it != soundMap.end()) {
		// sounditem found
		return it->second;
	}

	const auto itemDefIt = soundItemDefsMap.find(StringToLower(name));

	if (itemDefIt != soundItemDefsMap.end())
		return MakeItemFromDef(itemDefIt->second);

	if (LoadSoundBuffer(name) > 0) {
		// maybe raw filename?
		SoundItemNameMap temp = defaultItemNameMap;
		temp["file"] = name;
		return MakeItemFromDef(temp);
	}

	LOG_L(L_ERROR, "CSound::GetSoundId: could not find sound: %s", name.c_str());
	return 0;
}

SoundItem* CSound::GetSoundItem(size_t id) const {
	// id==0 is a special id and invalid
	if (id == 0 || id >= soundItems.size())
		return nullptr;

	// WARNING:
	//   leaked to SoundSource::PlayAsync via AudioChannel::FindSourceAndPlay
	//   soundItems vector grows on-demand via GetSoundId -> MakeItemFromDef
	return soundItems[id];
}

CSoundSource* CSound::GetNextBestSource(bool lock)
{
	std::unique_lock<spring::recursive_mutex> lck(soundMutex, std::defer_lock);
	if (lock)
		lck.lock();

	if (soundSources.empty())
		return nullptr;

	// find a free source; pointer remains valid until thread exits
	for (CSoundSource& src: soundSources) {
		if (!src.IsPlaying(false)) {
			return &src;
		}
	}

	// check the next best free source
	CSoundSource* bestSrc = nullptr;
	int bestPriority = INT_MAX;

	for (CSoundSource& src: soundSources) {
		#if 0
		if (!src.IsPlaying(true))
			return &src;
		#endif
		if (src.GetCurrentPriority() <= bestPriority) {
			bestSrc = &src;
			bestPriority = src.GetCurrentPriority();
		}
	}

	return bestSrc;
}

void CSound::PitchAdjust(const float newPitch)
{
	std::lock_guard<spring::recursive_mutex> lck(soundMutex);
	if (pitchAdjust)
		CSoundSource::SetPitch(newPitch);
}

void CSound::ConfigNotify(const std::string& key, const std::string& value)
{
	std::lock_guard<spring::recursive_mutex> lck(soundMutex);
	if (key == "snd_volmaster")
	{
		masterVolume = std::atoi(value.c_str()) * 0.01f;
		if (!mute && !appIsIconified)
			alListenerf(AL_GAIN, masterVolume);
	}
	else if (key == "snd_eaxpreset")
	{
		efx->SetPreset(value);
	}
	else if (key == "snd_filter")
	{
		float gainlf = 1.0f;
		float gainhf = 1.0f;
		sscanf(value.c_str(), "%f %f", &gainlf, &gainhf);
		efx->sfxProperties.filter_props_f[AL_LOWPASS_GAIN]   = gainlf;
		efx->sfxProperties.filter_props_f[AL_LOWPASS_GAINHF] = gainhf;
		efx->CommitEffects();
	}
	else if (key == "UseEFX")
	{
		bool enable = (std::atoi(value.c_str()) != 0);
		if (enable)
			efx->Enable();
		else
			efx->Disable();
	}
	else if (key == "snd_volgeneral")
	{
		Channels::General->SetVolume(std::atoi(value.c_str()) * 0.01f);
	}
	else if (key == "snd_volunitreply")
	{
		Channels::UnitReply->SetVolume(std::atoi(value.c_str()) * 0.01f);
	}
	else if (key == "snd_volbattle")
	{
		Channels::Battle->SetVolume(std::atoi(value.c_str()) * 0.01f);
	}
	else if (key == "snd_volui")
	{
		Channels::UserInterface->SetVolume(std::atoi(value.c_str()) * 0.01f);
	}
	else if (key == "snd_volmusic")
	{
		Channels::BGMusic->SetVolume(std::atoi(value.c_str()) * 0.01f);
	}
	else if (key == "PitchAdjust")
	{
		bool tempPitchAdjust = (std::atoi(value.c_str()) != 0);
		if (!tempPitchAdjust)
			PitchAdjust(1.0);
		pitchAdjust = tempPitchAdjust;
	}
}

bool CSound::Mute()
{
	std::lock_guard<spring::recursive_mutex> lck(soundMutex);
	mute = !mute;

	if (mute)
		alListenerf(AL_GAIN, 0.0f);
	else
		alListenerf(AL_GAIN, masterVolume);

	return mute;
}

bool CSound::IsMuted() const
{
	return mute;
}

void CSound::Iconified(bool state)
{
	std::lock_guard<spring::recursive_mutex> lck(soundMutex);

	if (appIsIconified != state && !mute) {
		if (!state)
			alListenerf(AL_GAIN, masterVolume);
		else
			alListenerf(AL_GAIN, 0.0);
	}

	appIsIconified = state;
}


void CSound::InitThread(int maxSounds)
{
	assert(maxSounds > 0);

	{
		std::lock_guard<spring::recursive_mutex> lck(soundMutex);

		// alc... will create its own thread it will copy the name from the current thread.
		// Later we finally rename `our` audio thread.
		Threading::SetThreadName("openal");

		// NULL -> default device
		const ALchar* deviceName = nullptr;
		std::string configDeviceName = "";

		// we do not want to set a default for snd_device,
		// so we do it like this ...
		if (configHandler->IsSet("snd_device")) {
			configDeviceName = configHandler->GetString("snd_device");
			deviceName = configDeviceName.c_str();
		}

		ALCdevice* device = alcOpenDevice(deviceName);

		if ((device == nullptr) && (deviceName != nullptr)) {
			LOG_L(L_WARNING, "[Sound::%s] could not open the sound device \"%s\", trying the default device ...", __func__, deviceName);
			configDeviceName = "";
			deviceName = nullptr;
			device = alcOpenDevice(deviceName);
		}

		if (device == nullptr) {
			LOG_L(L_ERROR, "[%s] could not open a sound device, disabling sounds", __func__);
			CheckError("CSound::InitAL");
			// fall back to NullSound
			soundThreadQuit = true;
			return;
		} else {
			ALCcontext* context = alcCreateContext(device, nullptr);

			if (context != nullptr) {
				alcMakeContextCurrent(context);
				CheckError("CSound::CreateContext");
			} else {
				alcCloseDevice(device);
				LOG_L(L_ERROR, "[Sound::%s] could not create OpenAL audio context", __func__);
				// fall back to NullSound
				soundThreadQuit = true;
				return;
			}
		}

		maxSounds = GetMaxMonoSources(device, maxSounds);

		{
			LOG("[Sound::%s] OpenAL info:", __func__);

			const bool hasAllEnum = alcIsExtensionPresent(nullptr, "ALC_ENUMERATE_ALL_EXT");
			const bool hasEnumExt = alcIsExtensionPresent(nullptr, "ALC_ENUMERATION_EXT");

			if (hasAllEnum || hasEnumExt) {
				LOG("  Available Devices:");
				const char* deviceSpecifier = alcGetString(nullptr, hasAllEnum ? ALC_ALL_DEVICES_SPECIFIER : ALC_DEVICE_SPECIFIER);

				while (*deviceSpecifier != '\0') {
					LOG("              %s", deviceSpecifier);
					while (*deviceSpecifier++ != '\0');
				}

				LOG("  Device:     %s", (const char*)alcGetString(device, ALC_DEVICE_SPECIFIER));
			}

			LOG("  Vendor:         %s", (const char*)alGetString(AL_VENDOR));
			LOG("  Version:        %s", (const char*)alGetString(AL_VERSION));
			LOG("  Renderer:       %s", (const char*)alGetString(AL_RENDERER));
			LOG("  AL Extensions:  %s", (const char*)alGetString(AL_EXTENSIONS));
			LOG("  ALC Extensions: %s", (const char*)alcGetString(device, ALC_EXTENSIONS));
		}
		{
			// generate sound sources; after this <soundSources> never changes size
			soundSources.clear();
			soundSources.reserve(maxSounds);

			for (int i = 0; i < maxSounds; i++) {
				soundSources.emplace_back();

				if (!soundSources[i].IsValid()) {
					soundSources.pop_back();
					maxSounds = soundSources.size();

					LOG_L(L_WARNING, "[Sound::%s] your hardware/driver can not handle more than %i soundsources", __func__, maxSounds);
					break;
				}
			}

			LOG("  Max Sounds: %i", maxSounds);
		}

		efx = new CEFX(device);

		// Set distance model (sound attenuation)
		alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
		alDopplerFactor(0.2f);

		alListenerf(AL_GAIN, masterVolume);
	}

	canLoadDefs = true;
}

__FORCE_ALIGN_STACK__
void CSound::UpdateThread(int maxSounds)
{
	InitThread(maxSounds);

	LOG("[Sound::%s][1] maxSounds=%d", __func__, maxSounds);

	Threading::SetThreadName("audio");
	Watchdog::RegisterThread(WDT_AUDIO);

	LOG("[Sound::%s][2]", __func__);

	while (!soundThreadQuit) {
		constexpr int FREQ_IN_HZ = 30;
		spring::this_thread::sleep_for(std::chrono::milliseconds(1000 / FREQ_IN_HZ));
		Watchdog::ClearTimer(WDT_AUDIO);
		Update();
	}

	Watchdog::DeregisterThread(WDT_AUDIO);

	LOG("[Sound::%s][3] efx=%p", __func__, efx);

	soundSources.clear();

	// must happen after sources and before context
	spring::SafeDelete(efx);

	ALCcontext* curContext = alcGetCurrentContext();
	ALCdevice* curDevice = alcGetContextsDevice(curContext);

	LOG("[Sound::%s][4]", __func__);

	alcMakeContextCurrent(nullptr);
	alcDestroyContext(curContext);
	alcCloseDevice(curDevice);
}

void CSound::Update()
{
	std::lock_guard<spring::recursive_mutex> lck(soundMutex); // lock

	for (CSoundSource& source: soundSources)
		source.Update();

	CheckError("CSound::Update");
	UpdateListenerReal();
}

size_t CSound::MakeItemFromDef(const SoundItemNameMap& itemDef)
{
	//! MakeItemFromDef is private. Only caller is LoadSoundDefs and it sets the mutex itself.
	// std::lock_guard<spring::recursive_mutex> lck(soundMutex);
	const size_t newid = soundItems.size();
	const auto defIt = itemDef.find("file");

	if (defIt == itemDef.end())
		return 0;

	std::shared_ptr<SoundBuffer> buffer = SoundBuffer::GetById(LoadSoundBuffer(defIt->second));

	if (!buffer)
		return 0;

	soundItems.push_back(new SoundItem(buffer, itemDef));
	soundMap[(soundItems.back())->Name()] = newid;
	return newid;
}

void CSound::UpdateListener(const float3& campos, const float3& camdir, const float3& camup)
{
	myPos  = campos;
	camDir = camdir;
	camUp  = camup;
	listenerNeedsUpdate = true;
}


void CSound::UpdateListenerReal()
{
	// call from sound thread, cause OpenAL calls tend to cause L2 misses and so are slow (no reason to call them from mainthread)
	if (!listenerNeedsUpdate)
		return;

	// not 100% threadsafe, but worst case we would skip a single listener update (and it runs at multiple Hz!)
	listenerNeedsUpdate = false;

	const float3 myPosInMeters = myPos * ELMOS_TO_METERS;
	alListener3f(AL_POSITION, myPosInMeters.x, myPosInMeters.y, myPosInMeters.z);

	// reduce the rolloff when the camera is high above the ground (so we still hear something in tab mode or far zoom)
	// for altitudes up to and including 600 elmos, the rolloff is always clamped to 1
	const float camHeight = std::max(1.0f, myPos.y - CGround::GetHeightAboveWater(myPos.x, myPos.z));
	const float newMod = std::min(600.0f / camHeight, 1.0f);

	CSoundSource::SetHeightRolloffModifer(newMod);
	efx->SetHeightRolloffModifer(newMod);

	// Result were bad with listener related doppler effects.
	// The user experiences the camera/listener not as a world-interacting object.
	// So changing sounds on camera movements were irritating, esp. because zooming with the mouse wheel
	// often is faster than the speed of sound, causing very high frequencies.
	// Note: soundsource related doppler effects are not deactivated by this! Flying cannon shoots still change their frequencies.
	// Note2: by not updating the listener velocity soundsource related velocities are calculated wrong,
	// so even if the camera is moving with a cannon shoot the frequency gets changed.
	/*
	const float3 velocity = (myPos - prevPos) / (lastFrameTime);
	float3 velocityAvg = velocity * 0.6f + prevVelocity * 0.4f;
	prevVelocity = velocityAvg;
	velocityAvg *= ELMOS_TO_METERS;
	velocityAvg.y *= 0.001f; //! scale vertical axis separatly (zoom with mousewheel is faster than speed of sound!)
	velocityAvg *= 0.15f;
	alListener3f(AL_VELOCITY, velocityAvg.x, velocityAvg.y, velocityAvg.z);
	*/

	ALfloat ListenerOri[] = {camDir.x, camDir.y, camDir.z, camUp.x, camUp.y, camUp.z};
	alListenerfv(AL_ORIENTATION, ListenerOri);
	CheckError("CSound::UpdateListener");
}


void CSound::PrintDebugInfo()
{
	std::lock_guard<spring::recursive_mutex> lck(soundMutex);

	LOG_L(L_DEBUG, "OpenAL Sound System:");
	LOG_L(L_DEBUG, "# SoundSources: %i", (int)soundSources.size());
	LOG_L(L_DEBUG, "# SoundBuffers: %i", (int)SoundBuffer::Count());

	LOG_L(L_DEBUG, "# reserved for buffers: %i kB", (int)(SoundBuffer::AllocedSize() / 1024));
	LOG_L(L_DEBUG, "# PlayRequests for empty sound: %i", numEmptyPlayRequests);
	LOG_L(L_DEBUG, "# Samples disrupted: %i", numAbortedPlays);
	LOG_L(L_DEBUG, "# SoundItems: %i", (int)soundItems.size());
}

bool CSound::LoadSoundDefsImpl(const std::string& fileName, const std::string& modes)
{
	//! can be called from LuaUnsyncedCtrl too
	std::lock_guard<spring::recursive_mutex> lck(soundMutex);

	LuaParser parser(fileName, modes, modes);
	parser.Execute();

	if (!parser.IsValid()) {
		LOG_L(L_WARNING, "Could not load %s: %s", fileName.c_str(), parser.GetErrorLog().c_str());
		return false;
	}

	{
		const LuaTable& soundRoot = parser.GetRoot();
		const LuaTable& soundItemTable = soundRoot.SubTable("SoundItems");

		if (!soundItemTable.IsValid()) {
			LOG_L(L_WARNING, "CSound(): could not parse SoundItems table in %s", fileName.c_str());
			return false;
		}

		{
			std::vector<std::string> keys;
			soundItemTable.GetKeys(keys);

			for (const std::string& name: keys) {
				SoundItemNameMap bufmap;
				const LuaTable buf(soundItemTable.SubTable(name));
				buf.GetMap(bufmap);
				bufmap["name"] = name;
				const auto sit = soundItemDefsMap.find(name);

				if (name == "default") {
					defaultItemNameMap = bufmap;
					defaultItemNameMap.erase("name"); //must be empty for default item
					defaultItemNameMap.erase("file");
					continue;
				}

				if (sit != soundItemDefsMap.end())
					LOG_L(L_WARNING, "Sound %s gets overwritten by %s", name.c_str(), fileName.c_str());

				if (!buf.KeyExists("file")) {
					// no file, drop
					LOG_L(L_WARNING, "Sound %s is missing file tag (ignoring)", name.c_str());
					continue;
				}

				soundItemDefsMap[name] = bufmap;

				if (buf.KeyExists("preload")) {
					MakeItemFromDef(bufmap);
				}
			}
			LOG(" parsed %i sounds from %s", (int)keys.size(), fileName.c_str());
		}
	}

	//FIXME why do sounds w/o an own soundItemDef create (!=pointer) a new one from the defaultItemNameMap?
	for (auto it = soundItemDefsMap.begin(); it != soundItemDefsMap.end(); ++it) {
		SoundItemNameMap& snddef = it->second;

		if (snddef.find("name") == snddef.end()) {
			// uses defaultItemNameMap! update it!
			const std::string file = snddef["file"];

			snddef = defaultItemNameMap;
			snddef["file"] = file;
		}
	}

	return true;
}

//! only used internally, locked in caller's scope
size_t CSound::LoadSoundBuffer(const std::string& path)
{
	const size_t id = SoundBuffer::GetId(path);

	if (id > 0)
		return id; // file is loaded already

	CFileHandler file(path);

	if (!file.FileExists()) {
		LOG_L(L_ERROR, "Unable to open audio file: %s", path.c_str());
		return 0;
	}

	std::vector<std::uint8_t> buf(file.FileSize());
	file.Read(&buf[0], file.FileSize());

	std::shared_ptr<SoundBuffer> buffer(new SoundBuffer());
	bool success = false;
	const std::string ending = file.GetFileExt();

	if (ending == "wav") {
		success = buffer->LoadWAV(path, buf);
	} else if (ending == "ogg") {
		success = buffer->LoadVorbis(path, buf);
	} else {
		LOG_L(L_WARNING, "CSound::LoadALBuffer: unknown audio format: %s", ending.c_str());
	}

	CheckError("CSound::LoadALBuffer");
	if (!success) {
		LOG_L(L_WARNING, "Failed to load file: %s", path.c_str());
		return 0;
	}

	return SoundBuffer::Insert(buffer);
}

void CSound::NewFrame()
{
	Channels::General->UpdateFrame();
	Channels::Battle->UpdateFrame();
	Channels::UnitReply->UpdateFrame();
	Channels::UserInterface->UpdateFrame();
}


// try to get the maximum number of supported sounds, this is similar to code CSound::UpdateThread
// but should be more safe
int CSound::GetMaxMonoSources(ALCdevice* device, int maxSounds)
{
	ALCint size;
	alcGetIntegerv(device, ALC_ATTRIBUTES_SIZE, 1, &size);
	std::vector<ALCint> attrs(size);
	alcGetIntegerv(device, ALC_ALL_ATTRIBUTES, size, &attrs[0]);

	for (int i = 0; i < attrs.size(); ++i) {
		if (attrs[i] != ALC_MONO_SOURCES)
			continue;

		const int maxMonoSources = attrs.at(i + 1);

		if (maxMonoSources < maxSounds)
			LOG_L(L_WARNING, "Hardware supports only %d Sound sources, MaxSounds=%d, using Hardware Limit", maxMonoSources, maxSounds);

		return std::min(maxSounds, maxMonoSources);
	}

	return maxSounds;
}

