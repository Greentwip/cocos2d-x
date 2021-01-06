/*
* cocos2d-x   http://www.cocos2d-x.org
*
* Copyright (c) 2010-2011 - cocos2d-x community
*
* Portions Copyright (c) Microsoft Open Technologies, Inc.
* All Rights Reserved
*
* Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software distributed under the License is distributed on an
* "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and limitations under the License.
*/

#include "base/ccMacros.h"
#include "platform/CCPlatformConfig.h"

#if CC_TARGET_PLATFORM == CC_PLATFORM_WINRT

#include "audio/winrt/AudioEngine-winrt.h"
#include "platform/CCFileUtils.h"
#include "base/CCDirector.h"
#include "base/CCScheduler.h"

using namespace cocos2d;
using namespace cocos2d::experimental;

AudioEngineImpl::AudioEngineImpl()
    : _lazyInitLoop(true)
    , _currentAudioID(0)
    , _cacheSize(16)
    , _playerCache()
{
    FillPlayerCache();

    auto scheduler = cocos2d::Director::getInstance()->getScheduler();
    scheduler->schedule(schedule_selector(AudioEngineImpl::update), this, 1.0f / 60.0f, false);
}

AudioEngineImpl::~AudioEngineImpl()
{
    auto scheduler = cocos2d::Director::getInstance()->getScheduler();
    scheduler->unschedule(schedule_selector(AudioEngineImpl::update), this);
    _audioCaches.clear();

    for (int i = 0; i < _playerCache.size(); ++i) {
        delete _playerCache[i];
    }
}

bool AudioEngineImpl::init()
{
    return true;
}

AudioCache* AudioEngineImpl::preload(const std::string& filePath, std::function<void(bool)> callback)
{
    AudioCache* audioCache = nullptr;
    do 
    {
        auto it = _audioCaches.find(filePath);
        if (it == _audioCaches.end()) {
            FileFormat fileFormat = FileFormat::UNKNOWN;

            std::string fileExtension = FileUtils::getInstance()->getFileExtension(filePath);

            if (fileExtension == ".wav")
            {
                fileFormat = FileFormat::WAV;
            }
            else if (fileExtension == ".ogg")
            {
                fileFormat = FileFormat::OGG;
            }
            else if (fileExtension == ".mp3")
            {
                fileFormat = FileFormat::MP3;
            }
            else
            {
                log("Unsupported media type file: %s\n", filePath.c_str());
                break;
            }

            audioCache = &_audioCaches[filePath];
            audioCache->_fileFormat = fileFormat;

            std::string fullPath = FileUtils::getInstance()->fullPathForFilename(filePath);
            audioCache->_fileFullPath = fullPath;
            AudioEngine::addTask(std::bind(&AudioCache::readDataTask, audioCache));
        }
        else 
        {
            audioCache = &it->second;
        }
    } while (false);

    if (callback)
    {
        if (audioCache)
        {
            audioCache->addLoadCallback(callback);
        } 
        else
        {
            callback(false);
        }
    }

    return audioCache;
}

int AudioEngineImpl::play2d(const std::string &filePath, bool loop, float volume)
{
    auto player = TakePlayerCache();
    if (player == nullptr) {
        return AudioEngine::INVALID_AUDIO_ID;
    }

    player->_loop = loop;
    player->_volume = volume;

    auto audioCache = preload(filePath, nullptr);
    if (audioCache == nullptr) {
        delete player;
        return AudioEngine::INVALID_AUDIO_ID;
    }

    _threadMutex.lock();
    _audioPlayers[_currentAudioID] = player;
    _threadMutex.unlock();

    audioCache->addPlayCallback(std::bind(&AudioEngineImpl::_play2d, this, audioCache, _currentAudioID));

    /*
    if (_lazyInitLoop) {
        _lazyInitLoop = false;

        auto scheduler = cocos2d::Director::getInstance()->getScheduler();
        scheduler->schedule(schedule_selector(AudioEngineImpl::update), this, 1.0f / 60.0f, false);
    }*/

    return _currentAudioID++;
}

void AudioEngineImpl::_play2d(AudioCache *cache, int audioID)
{
    if (cache->_isReady){
        auto playerIt = _audioPlayers.find(audioID);
        if (playerIt != _audioPlayers.end()) {
            if (playerIt->second->play2d(cache)) {
                AudioEngine::_audioIDInfoMap[audioID].state = AudioEngine::AudioState::PLAYING;
            }
            else{
                _threadMutex.lock();
                _toRemoveAudioIDs.push_back(audioID);
                _threadMutex.unlock();
            }
        }
    }
    else {
        _threadMutex.lock();
        _toRemoveCaches.push_back(cache);
        _toRemoveAudioIDs.push_back(audioID);
        _threadMutex.unlock();
    }
}

void AudioEngineImpl::setVolume(int audioID, float volume)
{
    auto& player = _audioPlayers[audioID];

    if (player->_ready){
        player->setVolume(volume);
    }

    if (player->isInError()) {
        log("%s: audio id = %d, error.\n", __FUNCTION__, audioID);
    }
}

void AudioEngineImpl::setLoop(int audioID, bool loop)
{
    auto& player = _audioPlayers[audioID];

    if (player->_ready) {
        player->_loop = loop;
    }

    if (player->isInError()) {
        log("%s: audio id = %d, error.\n", __FUNCTION__, audioID);
    }
}

bool AudioEngineImpl::pause(int audioID)
{
    bool ret = false;
    auto& player = _audioPlayers[audioID];

    if (player->_ready) {
        player->pause();
        AudioEngine::_audioIDInfoMap[audioID].state = AudioEngine::AudioState::PAUSED;
    }

    ret = !player->isInError();

    if (!ret) {
        log("%s: audio id = %d, error.\n", __FUNCTION__, audioID);
    }

    return ret;
}

bool AudioEngineImpl::resume(int audioID)
{
    bool ret = false;
    auto& player = _audioPlayers[audioID];

    if (player->_ready) {
        player->resume();
        AudioEngine::_audioIDInfoMap[audioID].state = AudioEngine::AudioState::PLAYING;
    }

    ret = !player->isInError();

    if (!ret) {
        log("%s: audio id = %d, error.\n", __FUNCTION__, audioID);
    }

    return ret;
}

bool AudioEngineImpl::stop(int audioID)
{
    bool ret = false;
    auto& player = _audioPlayers[audioID];

    if (player->_ready) {
        player->stop();
        ret = !player->isInError();
    }

    if (!ret) {
        log("%s: audio id = %d, error.\n", __FUNCTION__, audioID);
    }

    _audioPlayers.erase(audioID);
    return ret;
}

void AudioEngineImpl::stopAll()
{
    for (auto &player : _audioPlayers) {
        player.second->stop();
    }

    _audioPlayers.clear();
}

float AudioEngineImpl::getDuration(int audioID)
{
    auto& player = _audioPlayers[audioID];

    if (player->_ready) {
        return player->getDuration();
    }
    else {
        return AudioEngine::TIME_UNKNOWN;
    }
}

float AudioEngineImpl::getCurrentTime(int audioID)
{
    float ret = 0.0f;
    auto& player = _audioPlayers[audioID];

    if (player->_ready) {
        ret = player->getCurrentTime();
    }

    return ret;
}

bool AudioEngineImpl::setCurrentTime(int audioID, float time)
{
    bool ret = false;
    auto& player = _audioPlayers[audioID];

    if (player->_ready) {
        ret = player->setTime(time);
    }

    if (!ret) {
        log("%s: audio id = %d, error.\n", __FUNCTION__, audioID);
    }

    return ret;
}

void AudioEngineImpl::setFinishCallback(int audioID, const std::function<void(int, const std::string &)> &callback)
{
    _audioPlayers[audioID]->_finishCallback = callback;
}

void AudioEngineImpl::FillPlayerCache() {
    _cacheMutex.lock();
    if (_playerCache.size() < _cacheSize) {
        for (int i = 0; i < _cacheSize - _playerCache.size(); ++i) {
            _playerCache.push_back(new AudioPlayer());
        }
    }    
    _cacheMutex.unlock();

}

AudioPlayer* AudioEngineImpl::TakePlayerCache() {
    AudioPlayer* playerBackup = nullptr;
    _cacheMutex.lock();
    if (!_playerCache.empty()) {
        playerBackup = _playerCache.back();
        _playerCache.pop_back();
    }
    _cacheMutex.unlock();

    return playerBackup;
}

void AudioEngineImpl::update(float dt)
{
    AudioEngine::addTask(std::bind(&AudioEngineImpl::FillPlayerCache, this));

    int audioID;

    if (_threadMutex.try_lock()) {
        size_t removeAudioCount = _toRemoveAudioIDs.size();
        for (size_t index = 0; index < removeAudioCount; ++index) {
            audioID = _toRemoveAudioIDs[index];
            auto playerIt = _audioPlayers.find(audioID);
            auto player = playerIt->second;
            if (playerIt != _audioPlayers.end()) {
                if (playerIt->second->_finishCallback) {
                    auto& audioInfo = AudioEngine::_audioIDInfoMap[audioID];
                    playerIt->second->_finishCallback(audioID, *audioInfo.filePath);
                }
                _threadMutex.lock();
                _audioPlayers.erase(audioID);
                _threadMutex.unlock();
                delete player;

                AudioEngine::remove(audioID);

            }
        }

        size_t removeCacheCount = _toRemoveCaches.size();
        for (size_t index = 0; index < removeCacheCount; ++index) {
            auto itEnd = _audioCaches.end();
            for (auto it = _audioCaches.begin(); it != itEnd; ++it) {
                if (&it->second == _toRemoveCaches[index]) {
                    _audioCaches.erase(it);
                    break;
                }
            }
        }
        _threadMutex.unlock();
    }

    for (auto it = _audioPlayers.begin(); it != _audioPlayers.end();) {
        audioID = it->first;
        auto player = it->second;

        if (player->_ready && player->_state == AudioPlayerState::STOPPED) {
            if (player->_finishCallback) {
                auto& audioInfo = AudioEngine::_audioIDInfoMap[audioID];
                player->_finishCallback(audioID, *audioInfo.filePath);
            }

            AudioEngine::remove(audioID);

            _threadMutex.lock();
            it = _audioPlayers.erase(it);
            _threadMutex.unlock();

            delete player;

        }
        else{
            player->update();
            ++it;
        }
    }

    /*
    if (_audioPlayers.empty()){
        _lazyInitLoop = true;

        auto scheduler = cocos2d::Director::getInstance()->getScheduler();
        scheduler->unschedule(schedule_selector(AudioEngineImpl::update), this);
    }*/
}

void AudioEngineImpl::uncache(const std::string &filePath)
{
    _audioCaches.erase(filePath);
}

void AudioEngineImpl::uncacheAll()
{
    _audioCaches.clear();
}

#endif
