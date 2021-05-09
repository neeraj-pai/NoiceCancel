#pragma comment(lib,"winmm.lib")

#include <Windows.h>
#include <mmsystem.h>
#include <fstream>
#include <iostream>

//Noise cancelling application
//System design
//1 thread to run capture from mic -> Input Queue
//1 thread to run Input Quueue -> noise cancel -> Output Queue
//1 thread to run playback -> Output Queue

int nReadBufferCnt = 0;
int nWriteBufferCnt = 0;

#define SAMPLE_RATE 44100
#define NUM_CALLBACK_BUFFERS 4
#define NUM_CHANNELS 2
#define BYTES_OUT 8
#define THREADCOUNT 4

char nReadCaptureBuffers[NUM_CALLBACK_BUFFERS][SAMPLE_RATE * BYTES_OUT/8 * NUM_CHANNELS];
char nWriteCaptureBuffers[NUM_CALLBACK_BUFFERS][SAMPLE_RATE * BYTES_OUT/8 * NUM_CHANNELS];
WAVEHDR capture_headers[NUM_CALLBACK_BUFFERS];
WAVEHDR playback_headers[NUM_CALLBACK_BUFFERS];
HWAVEIN wi;
HWAVEOUT wo;

//need to use counting semaphore
//producer thread or callback tells us that a new capture buffer is ready
//consumer thread will do the processing and then send it to playback
//to reduce dependence chain input i buffer to output i buffer

//how do we know which of nread is ready?
//order in which callbacks come - assume 0,1,2,3
//increment counter in critical section so variable incremented in correct order

CONDITION_VARIABLE BufferNotEmpty;
CRITICAL_SECTION   BufferLock;
BOOL bContinue;

//4 threads since we have 4 processors
DWORD WINAPI myProcessThread(LPVOID lpParameter)
{
	// lpParam not used in this example
	UNREFERENCED_PARAMETER(lpParameter);

	DWORD dwWaitResult = 0;
	

	int nReadCnt;
	int nWriteCnt;
	char *pReadPtr;
	char *pWritePtr;

	//thread never terminates
	while (bContinue)
	{
		EnterCriticalSection(&BufferLock);

		SleepConditionVariableCS(&BufferNotEmpty, &BufferLock, INFINITE);

		nReadCnt = nReadBufferCnt;
		nWriteCnt = nWriteBufferCnt;

		pReadPtr = nReadCaptureBuffers[nReadBufferCnt];
		pWritePtr = nWriteCaptureBuffers[nWriteBufferCnt];

		//std::cout << "Processing Buffer " << nReadBufferCnt << std::endl;

		nReadBufferCnt++;
		if (nReadBufferCnt == NUM_CALLBACK_BUFFERS)
		{
			nReadBufferCnt = 0;
		}

		nWriteBufferCnt++;
		if (nWriteBufferCnt == NUM_CALLBACK_BUFFERS)
		{
			nWriteBufferCnt = 0;
		}

		LeaveCriticalSection(&BufferLock);

		//eventually process for now do memcpy
		memcpy(pWritePtr, pReadPtr, SAMPLE_RATE * NUM_CHANNELS * BYTES_OUT/8);

		//submit write buffer to be played back
		//waveOutPrepareHeader(wo, &playback_headers[nWriteCnt], sizeof(playback_headers[nWriteCnt]));
		waveOutPrepareHeader(wo, &playback_headers[nWriteCnt], sizeof(WAVEHDR));		
		waveOutWrite(wo, &playback_headers[nWriteCnt], SAMPLE_RATE * BYTES_OUT / 8 * NUM_CHANNELS);

		//submit read buffer back to queue
		capture_headers[nReadCnt].dwFlags = 0;          // clear the 'done' flag
		capture_headers[nReadCnt].dwBytesRecorded = 0;  // tell it no bytes have been recorded

		//waveInPrepareHeader(wi, &capture_headers[nReadCnt], sizeof(capture_headers[nReadCnt]));
		waveInPrepareHeader(wi, &capture_headers[nReadCnt], sizeof(WAVEHDR));
		waveInAddBuffer(wi, &capture_headers[nReadCnt], SAMPLE_RATE * BYTES_OUT / 8 * NUM_CHANNELS);
	}

	return(dwWaitResult);
}

void CALLBACK PlaybackCallBackThread(
	HWAVEOUT  hwo,
	UINT      uMsg,
	DWORD_PTR dwInstance,
	DWORD_PTR dwParam1,
	DWORD_PTR dwParam2
)
{
	switch (uMsg)
	{
	case WOM_CLOSE:
		std::cout << "playback call back exiting" << std::endl;
		break;
	case WOM_OPEN:
		std::cout << "playback call back registered" << std::endl;
		break;
	case WOM_DONE:
		//std::cout << "data played back, now record" << std::endl;
		break;
	}
}

void CALLBACK RecordCallBackThread(
	HWAVEIN   hwi,
	UINT      uMsg,
	DWORD_PTR dwInstance,
	DWORD_PTR dwParam1,
	DWORD_PTR dwParam2
)
{
	switch (uMsg)
	{
	case WIM_CLOSE:
		std::cout << "record call back exiting" << std::endl;
		break;
	case WIM_OPEN:
		std::cout << "record call back registered" << std::endl;
		break;
	case WIM_DATA:
		//std::cout << "record data recieved, playback" << std::endl;
		//signal using condition variables
		WakeAllConditionVariable(&BufferNotEmpty);
		break;
	}
}


void RunThreadWithCallBacks()
{
	//same handle used for both
	// Fill the WAVEFORMATEX struct to indicate the format of our recorded audio
	WAVEFORMATEX wfxRecord = {};
	wfxRecord.wFormatTag = WAVE_FORMAT_PCM;       // PCM is standard
	wfxRecord.nChannels = NUM_CHANNELS;                      // 2 channels = stereo
	wfxRecord.nSamplesPerSec = SAMPLE_RATE;       // Samplerate.  8 KHz
	wfxRecord.wBitsPerSample = BYTES_OUT;                // 16 bit samples
												  // These others are computations:
	wfxRecord.nBlockAlign = wfxRecord.wBitsPerSample * wfxRecord.nChannels / 8;
	wfxRecord.nAvgBytesPerSec = wfxRecord.nBlockAlign * wfxRecord.nSamplesPerSec;

	// Open our 'waveIn' recording device
	int Res = waveInOpen(&wi,            // fill our 'wi' handle
		WAVE_MAPPER,    // use default device (easiest)
		&wfxRecord,     // tell it our format
		(DWORD_PTR)((VOID*)&RecordCallBackThread),
		(DWORD_PTR)(&capture_headers),
		CALLBACK_FUNCTION | WAVE_FORMAT_DIRECT   // tell it we do not need a callback
	);

	// At this point, we have our device, now we need to give it buffers (with headers) that it can
	//  put the recorded audio somewhere
	for (int i = 0; i < NUM_CALLBACK_BUFFERS; ++i)
	{
		capture_headers[i].lpData = nReadCaptureBuffers[i];             // give it a pointer to our buffer
		capture_headers[i].dwBufferLength = SAMPLE_RATE;      // tell it the size of that buffer in bytes
															// the other parts of the header we don't really care about for this example, and can be left at zero

		// Prepare each header
		//waveInPrepareHeader(wi, &capture_headers[i], sizeof(capture_headers[i]));
		waveInPrepareHeader(wi, &capture_headers[i], SAMPLE_RATE * BYTES_OUT / 8 * NUM_CHANNELS);
		
		// And add it to the queue
		//  Once we start recording, queued buffers will get filled with audio data
		//waveInAddBuffer(wi, &capture_headers[i], sizeof(capture_headers[i]));
		waveInAddBuffer(wi, &capture_headers[i], SAMPLE_RATE * BYTES_OUT / 8 * NUM_CHANNELS);
	}

	//Open out waveout device
	Res = waveOutOpen(&wo,
		WAVE_MAPPER, &wfxRecord,
		(DWORD_PTR)((VOID*)&PlaybackCallBackThread),
		(DWORD_PTR)(&playback_headers),
		CALLBACK_FUNCTION | WAVE_FORMAT_DIRECT);

	
	for (int i = 0; i < NUM_CALLBACK_BUFFERS; ++i)
	{
		playback_headers[i].lpData = nWriteCaptureBuffers[i];             // give it a pointer to our buffer
		playback_headers[i].dwBufferLength = SAMPLE_RATE;      // tell it the size of that buffer in bytes
															 // the other parts of the header we don't really care about for this example, and can be left at zero
															 // Prepare each header
		//waveOutPrepareHeader(wo, &playback_headers[i], sizeof(playback_headers[i]));
		waveOutPrepareHeader(wo, &playback_headers[i], SAMPLE_RATE * BYTES_OUT / 8 * NUM_CHANNELS);
	}
	
	InitializeConditionVariable(&BufferNotEmpty);
	InitializeCriticalSection(&BufferLock);

	// Create 4 worker threads to process capture
	int i;
	HANDLE aThread[THREADCOUNT];
	DWORD ThreadID;

	bContinue = true;

	for (i = 0; i < THREADCOUNT; i++)
	{
		aThread[i] = CreateThread(
			NULL,       // default security attributes
			0,          // default stack size
			(LPTHREAD_START_ROUTINE)myProcessThread,
			NULL,       // no thread function arguments
			0,          // default creation flags
			&ThreadID); // receive thread identifier

		if (aThread[i] == NULL)
		{
			printf("CreateThread error: %d\n", GetLastError());
			return;
		}
	}

	// Print some simple directions to the user
	std::cout << "Now recording audio and looping back" << std::endl;

	// start recording!
	waveInStart(wi);

	while (!(GetAsyncKeyState(VK_ESCAPE) & 0x8000))  // keep looping until the user hits escape
	{

	}

	EnterCriticalSection(&BufferLock);
	bContinue = false;
	LeaveCriticalSection(&BufferLock);

	WakeAllConditionVariable(&BufferNotEmpty);
	WaitForMultipleObjects(THREADCOUNT, aThread, TRUE, INFINITE);

	for (i = 0; i < THREADCOUNT; i++)
	{
		CloseHandle(aThread[i]);
	}

	// Once the user hits escape, stop recording, and clean up
	waveInStop(wi);
	for (auto& h : capture_headers)
	{
		waveInUnprepareHeader(wi, &h, sizeof(h));
	}
	waveInClose(wi);

	for (auto& h : playback_headers)
	{
		waveOutUnprepareHeader(wo, &h, sizeof(h));
	}
	waveOutClose(wo);
	return;
}

#define NUM_BUFFERS 5
void InitializeNoCallBack()
{
	//Initialize record 

	//same handle used for both
	// Fill the WAVEFORMATEX struct to indicate the format of our recorded audio
	//   For this example we'll use "CD quality", ie:  44100 Hz, stereo, 16-bit
	WAVEFORMATEX wfx = {};
	wfx.wFormatTag = WAVE_FORMAT_PCM;       // PCM is standard
	wfx.nChannels = 2;                      // 2 channels = stereo sound
	wfx.nSamplesPerSec = 44100;             // Samplerate.  44100 Hz
	wfx.wBitsPerSample = 16;                // 16 bit samples
											// These others are computations:
	wfx.nBlockAlign = wfx.wBitsPerSample * wfx.nChannels / 8;
	wfx.nAvgBytesPerSec = wfx.nBlockAlign * wfx.nSamplesPerSec;


	// Open our 'waveIn' recording device
	HWAVEIN wi;
	int Res = waveInOpen(&wi,            // fill our 'wi' handle
		WAVE_MAPPER,    // use default device (easiest)
		&wfx,           // tell it our format
		NULL, NULL,      // we don't need a callback for this example
		WAVE_FORMAT_QUERY   // tell it we do not need a callback
	);

	if (Res == WAVERR_BADFORMAT)
	{
		return;
	}

	//or else open
	Res = waveInOpen(&wi,            // fill our 'wi' handle
		WAVE_MAPPER,    // use default device (easiest)
		&wfx,           // tell it our format
		NULL, NULL,      // we don't need a callback for this example
		CALLBACK_NULL | WAVE_FORMAT_DIRECT   // tell it we do not need a callback
	);

	HWAVEOUT WaveHandle;
	Res = waveOutOpen(&WaveHandle, WAVE_MAPPER, &wfx, 0, 0,
		WAVE_FORMAT_QUERY);
	if (Res == WAVERR_BADFORMAT)
	{
		return;
	}

	Res = waveOutOpen(&WaveHandle, WAVE_MAPPER, &wfx, 0, 0,
		CALLBACK_NULL | WAVE_FORMAT_DIRECT);

	//using 2 buffers for now
	char buffers[NUM_BUFFERS][44100 * 2 * 2 / 2];    // 2 buffers, each half of a second long

	WAVEHDR headers[NUM_BUFFERS] = { {},{} };           // initialize them to zeros
	for (int i = 0; i < NUM_BUFFERS; ++i)
	{
		headers[i].lpData = buffers[i];             // give it a pointer to our buffer
		headers[i].dwBufferLength = 44100 * 2 * 2 / 2;      // tell it the size of that buffer in bytes
															// the other parts of the header we don't really care about for this example, and can be left at zero

															// Prepare each header
		waveInPrepareHeader(wi, &headers[i], sizeof(headers[i]));

		// And add it to the queue
		//  Once we start recording, queued buffers will get filled with audio data
		waveInAddBuffer(wi, &headers[i], sizeof(headers[i]));
	}

	// start recording!
	waveInStart(wi);

	waveOutSetVolume(WaveHandle, 0xFFFFFFFF);

	WAVEHDR WaveHeader;
	memset(&WaveHeader, 0, sizeof(WaveHeader));

	WaveHeader.dwLoops = 1;
	WaveHeader.dwFlags =
		WHDR_BEGINLOOP | WHDR_ENDLOOP;

	//char test_out_buffers[44100 * 2 * 2 / 2];
	//memset(test_out_buffers, 0x7F, 44100 * 2);

	MMRESULT nOut;
	// Now that we are recording, keep polling our buffers to see if they have been filled.
	//   If they have been, dump their contents to the file and re-add them to the queue so they
	//   can get filled again, and again, and again
	while (!(GetAsyncKeyState(VK_ESCAPE) & 0x8000))  // keep looping until the user hits escape
	{
		for (auto& h : headers)      // check each header
		{
			if (h.dwFlags & WHDR_DONE)           // is this header done?
			{
				// if yes, playback
				WaveHeader.lpData = h.lpData; ;// test_out_buffers;// h.lpData; 
				WaveHeader.dwBufferLength = h.dwBufferLength;

				nOut = waveOutPrepareHeader(WaveHandle, &WaveHeader, sizeof(WAVEHDR));

				nOut = waveOutWrite(
					WaveHandle, &WaveHeader, sizeof
					(WAVEHDR));
				if (MMSYSERR_NOERROR == nOut)
				{
					Res = 0;
				}

				// then re-add it to the queue
				h.dwFlags = 0;          // clear the 'done' flag
				h.dwBytesRecorded = 0;  // tell it no bytes have been recorded

										// re-add it  (I don't know why you need to prepare it again though...)
				waveInPrepareHeader(wi, &h, sizeof(h));
				waveInAddBuffer(wi, &h, sizeof(h));
			}
		}
	}

	// Once the user hits escape, stop recording, and clean up
	waveInStop(wi);
	for (auto& h : headers)
	{
		waveInUnprepareHeader(wi, &h, sizeof(h));
	}
	waveInClose(wi);

	while ((WaveHeader.dwFlags & WHDR_DONE) == 0);

	waveOutUnprepareHeader(WaveHandle, &WaveHeader, sizeof(WAVEHDR));
	waveOutClose(WaveHandle);
}


/* Write program to record using mic and then playback using speakers*/
void main()
{
	//InitializeNoCallBack();
	RunThreadWithCallBacks();
	return;
}