//puncho - C3e-C: one XAudio2 source voice pulls the finished mixer stream.
#ifndef __XA2_SOUNDSINK_H__
#define __XA2_SOUNDSINK_H__

#include <atomic>

class idSoundHardware_Mixer;

class idSoundSink_XAudio2 : private IXAudio2EngineCallback, private IXAudio2VoiceCallback
{
public:
						idSoundSink_XAudio2();
	bool				Open( idSoundHardware_Mixer* owner );
	void				Close();
	bool				IsOpen() const { return pSourceVoice != NULL; }
	int					OutputChannels() const { return outputChannels; }
	int					ChannelMask() const { return channelMask; }
	bool				Lost();
	void				SetMasterVolume( float volume );
	int					LatencyFrames() const;
	IXAudio2*			GetIXAudio2() const { return pXAudio2; }
	static void			ListDevices( const idCmdArgs& args );

private:
	STDMETHOD_( void, OnProcessingPassStart )( THIS ) {}
	STDMETHOD_( void, OnProcessingPassEnd )( THIS ) {}
	STDMETHOD_( void, OnCriticalError )( THIS_ HRESULT error );
	STDMETHOD_( void, OnVoiceProcessingPassStart )( THIS_ UINT32 bytesRequired );
	STDMETHOD_( void, OnVoiceProcessingPassEnd )( THIS ) {}
	STDMETHOD_( void, OnStreamEnd )( THIS ) {}
	STDMETHOD_( void, OnBufferStart )( THIS_ void* context ) {}
	STDMETHOD_( void, OnBufferEnd )( THIS_ void* context ) {}
	STDMETHOD_( void, OnLoopEnd )( THIS_ void* context ) {}
	STDMETHOD_( void, OnVoiceError )( THIS_ void* context, HRESULT error ) {}

	idSoundHardware_Mixer*	owner;
	IXAudio2*			pXAudio2;
	IXAudio2MasteringVoice*	pMasterVoice;
	IXAudio2SourceVoice*	pSourceVoice;
	float*				buffers;
	int					nextBuffer;
	int					outputChannels;
	int					channelMask;
	bool				classicInitialized;
	std::atomic<bool>	lost;
};

#endif
