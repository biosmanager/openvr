//============ Copyright (c) Valve Corporation, All rights reserved. ============

#include <openvr_driver.h>
#include "threadtools.h"
#include "driverlog.h"


#include <vector>
#include <thread>
#include <atomic>
#include <chrono>

#if defined( _WINDOWS )
#include <windows.h>
#endif

using namespace vr;


#if defined(_WIN32)
#define HMD_DLL_EXPORT extern "C" __declspec( dllexport )
#define HMD_DLL_IMPORT extern "C" __declspec( dllimport )
#elif defined(__GNUC__) || defined(COMPILER_GCC) || defined(__APPLE__)
#define HMD_DLL_EXPORT extern "C" __attribute__((visibility("default")))
#define HMD_DLL_IMPORT extern "C" 
#else
#error "Unsupported Platform."
#endif

inline HmdQuaternion_t HmdQuaternion_Init( double w, double x, double y, double z )
{
	HmdQuaternion_t quat;
	quat.w = w;
	quat.x = x;
	quat.y = y;
	quat.z = z;
	return quat;
}

inline void HmdMatrix_SetIdentity( HmdMatrix34_t *pMatrix )
{
	pMatrix->m[0][0] = 1.f;
	pMatrix->m[0][1] = 0.f;
	pMatrix->m[0][2] = 0.f;
	pMatrix->m[0][3] = 0.f;
	pMatrix->m[1][0] = 0.f;
	pMatrix->m[1][1] = 1.f;
	pMatrix->m[1][2] = 0.f;
	pMatrix->m[1][3] = 0.f;
	pMatrix->m[2][0] = 0.f;
	pMatrix->m[2][1] = 0.f;
	pMatrix->m[2][2] = 1.f;
	pMatrix->m[2][3] = 0.f;
}


// keys for use with the settings API
static const char * const k_pch_UNCAPPED_Section = "driver_uncapped";
static const char * const k_pch_UNCAPPED_SerialNumber_String = "serialNumber";
static const char * const k_pch_UNCAPPED_ModelNumber_String = "modelNumber";
static const char * const k_pch_UNCAPPED_WindowX_Int32 = "windowX";
static const char * const k_pch_UNCAPPED_WindowY_Int32 = "windowY";
static const char * const k_pch_UNCAPPED_WindowWidth_Int32 = "windowWidth";
static const char * const k_pch_UNCAPPED_WindowHeight_Int32 = "windowHeight";
static const char * const k_pch_UNCAPPED_RenderWidth_Int32 = "renderWidth";
static const char * const k_pch_UNCAPPED_RenderHeight_Int32 = "renderHeight";
static const char * const k_pch_UNCAPPED_SecondsFromVsyncToPhotons_Float = "secondsFromVsyncToPhotons";
static const char * const k_pch_UNCAPPED_DisplayFrequency_Float = "displayFrequency";

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------

class CWatchdogDriver_UNCAPPED : public IVRWatchdogProvider
{
public:
	CWatchdogDriver_UNCAPPED()
	{
		m_pWatchdogThread = nullptr;
	}

	virtual EVRInitError Init( vr::IVRDriverContext *pDriverContext ) ;
	virtual void Cleanup() ;

private:
	std::thread *m_pWatchdogThread;
};

CWatchdogDriver_UNCAPPED g_watchdogDriverNull;


bool g_bExiting = false;

void WatchdogThreadFunction(  )
{
	while ( !g_bExiting )
	{
#if defined( _WINDOWS )
		// on windows send the event when the Y key is pressed.
		if ( (0x01 & GetAsyncKeyState( 'Y' )) != 0 )
		{
			// Y key was pressed. 
			vr::VRWatchdogHost()->WatchdogWakeUp( vr::TrackedDeviceClass_HMD );
		}
		std::this_thread::sleep_for( std::chrono::microseconds( 500 ) );
#else
		// for the other platforms, just send one every five seconds
		std::this_thread::sleep_for( std::chrono::seconds( 5 ) );
		vr::VRWatchdogHost()->WatchdogWakeUp( vr::TrackedDeviceClass_HMD );
#endif
	}
}

EVRInitError CWatchdogDriver_UNCAPPED::Init( vr::IVRDriverContext *pDriverContext )
{
	VR_INIT_WATCHDOG_DRIVER_CONTEXT( pDriverContext );
	InitDriverLog( vr::VRDriverLog() );

	// Watchdog mode on Windows starts a thread that listens for the 'Y' key on the keyboard to 
	// be pressed. A real driver should wait for a system button event or something else from the 
	// the hardware that signals that the VR system should start up.
	g_bExiting = false;
	m_pWatchdogThread = new std::thread( WatchdogThreadFunction );
	if ( !m_pWatchdogThread )
	{
		DriverLog( "Unable to create watchdog thread\n");
		return VRInitError_Driver_Failed;
	}

	return VRInitError_None;
}


void CWatchdogDriver_UNCAPPED::Cleanup()
{
	g_bExiting = true;
	if ( m_pWatchdogThread )
	{
		m_pWatchdogThread->join();
		delete m_pWatchdogThread;
		m_pWatchdogThread = nullptr;
	}

	CleanupDriverLog();
}	 


class CTiming : public CThread
{
public:
	virtual void Run() override {
		while (!m_doStop) {
			if (std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::high_resolution_clock::now() - lastVsyncTime).count() > 0.01) {
				lastVsyncTime = std::chrono::high_resolution_clock::now();
				m_frameCounter++;
			}
		}
	}

	double get_time_since_last_vsync() {
		return (std::chrono::high_resolution_clock::now() - lastVsyncTime).count() * 1e-9f;
	}

	uint64_t get_frame_counter() {
		return m_frameCounter;
	}

	void stop() {
		m_doStop = true;
		Join();
	}

private:
	bool m_doStop = false;
	uint64_t m_frameCounter = 0;
	std::chrono::time_point<std::chrono::high_resolution_clock> lastVsyncTime = std::chrono::high_resolution_clock::now();
};


//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
class CUNCAPPEDDeviceDriver : public vr::ITrackedDeviceServerDriver, public vr::IVRDisplayComponent, public vr::IVRVirtualDisplay //, public vr::IVRDriverDirectModeComponent
{
public:
	CUNCAPPEDDeviceDriver(  )
	{
		m_unObjectId = vr::k_unTrackedDeviceIndexInvalid;
		m_ulPropertyContainer = vr::k_ulInvalidPropertyContainer;

		DriverLog( "Using settings values\n" );
		m_flIPD = vr::VRSettings()->GetFloat( k_pch_SteamVR_Section, k_pch_SteamVR_IPD_Float );

		char buf[1024];
		vr::VRSettings()->GetString( k_pch_UNCAPPED_Section, k_pch_UNCAPPED_SerialNumber_String, buf, sizeof( buf ) );
		m_sSerialNumber = buf;

		vr::VRSettings()->GetString( k_pch_UNCAPPED_Section, k_pch_UNCAPPED_ModelNumber_String, buf, sizeof( buf ) );
		m_sModelNumber = buf;

		m_nWindowX = vr::VRSettings()->GetInt32( k_pch_UNCAPPED_Section, k_pch_UNCAPPED_WindowX_Int32 );
		m_nWindowY = vr::VRSettings()->GetInt32( k_pch_UNCAPPED_Section, k_pch_UNCAPPED_WindowY_Int32 );
		m_nWindowWidth = vr::VRSettings()->GetInt32( k_pch_UNCAPPED_Section, k_pch_UNCAPPED_WindowWidth_Int32 );
		m_nWindowHeight = vr::VRSettings()->GetInt32( k_pch_UNCAPPED_Section, k_pch_UNCAPPED_WindowHeight_Int32 );
		m_nRenderWidth = vr::VRSettings()->GetInt32( k_pch_UNCAPPED_Section, k_pch_UNCAPPED_RenderWidth_Int32 );
		m_nRenderHeight = vr::VRSettings()->GetInt32( k_pch_UNCAPPED_Section, k_pch_UNCAPPED_RenderHeight_Int32 );
		m_flSecondsFromVsyncToPhotons = vr::VRSettings()->GetFloat( k_pch_UNCAPPED_Section, k_pch_UNCAPPED_SecondsFromVsyncToPhotons_Float );
		m_flDisplayFrequency = vr::VRSettings()->GetFloat( k_pch_UNCAPPED_Section, k_pch_UNCAPPED_DisplayFrequency_Float );

		DriverLog( "driver_uncapped: Serial Number: %s\n", m_sSerialNumber.c_str() );
		DriverLog( "driver_uncapped: Model Number: %s\n", m_sModelNumber.c_str() );
		DriverLog( "driver_uncapped: Window: %d %d %d %d\n", m_nWindowX, m_nWindowY, m_nWindowWidth, m_nWindowHeight );
		DriverLog( "driver_uncapped: Render Target: %d %d\n", m_nRenderWidth, m_nRenderHeight );
		DriverLog( "driver_uncapped: Seconds from Vsync to Photons: %f\n", m_flSecondsFromVsyncToPhotons );
		DriverLog( "driver_uncapped: Display Frequency: %f\n", m_flDisplayFrequency );
		DriverLog( "driver_uncapped: IPD: %f\n", m_flIPD );

		//timingThread = new CTiming();
		//timingThread->Start();
	}

	virtual ~CUNCAPPEDDeviceDriver()
	{
		if (timingThread) {
			timingThread->stop();
			delete timingThread;
		}
	}


	virtual EVRInitError Activate( vr::TrackedDeviceIndex_t unObjectId ) 
	{
		m_unObjectId = unObjectId;
		m_ulPropertyContainer = vr::VRProperties()->TrackedDeviceToPropertyContainer( m_unObjectId );

		vr::VRProperties()->SetStringProperty( m_ulPropertyContainer, Prop_ModelNumber_String, m_sModelNumber.c_str() );
		vr::VRProperties()->SetStringProperty( m_ulPropertyContainer, Prop_RenderModelName_String, m_sModelNumber.c_str() );
		vr::VRProperties()->SetFloatProperty( m_ulPropertyContainer, Prop_UserIpdMeters_Float, m_flIPD );
		vr::VRProperties()->SetFloatProperty( m_ulPropertyContainer, Prop_UserHeadToEyeDepthMeters_Float, 0.f );
		vr::VRProperties()->SetFloatProperty( m_ulPropertyContainer, Prop_DisplayFrequency_Float, m_flDisplayFrequency );
		vr::VRProperties()->SetFloatProperty( m_ulPropertyContainer, Prop_SecondsFromVsyncToPhotons_Float, m_flSecondsFromVsyncToPhotons );

		// return a constant that's not 0 (invalid) or 1 (reserved for Oculus)
		vr::VRProperties()->SetUint64Property( m_ulPropertyContainer, Prop_CurrentUniverseId_Uint64, 2 );

		vr::VRProperties()->SetBoolProperty(m_ulPropertyContainer, Prop_DriverDirectModeSendsVsyncEvents_Bool, false);

		// Prevent fake HMD from going to standby
		vr::VRDriverInput()->CreateBooleanComponent(m_ulPropertyContainer, "/proximity", &m_proximitySensor);
		vr::VRDriverInput()->UpdateBooleanComponent(m_proximitySensor, true, 0.0);

		// Icons can be configured in code or automatically configured by an external file "drivername\resources\driver.vrresources".
		// Icon properties NOT configured in code (post Activate) are then auto-configured by the optional presence of a driver's "drivername\resources\driver.vrresources".
		// In this manner a driver can configure their icons in a flexible data driven fashion by using an external file.
		//
		// The structure of the driver.vrresources file allows a driver to specialize their icons based on their HW.
		// Keys matching the value in "Prop_ModelNumber_String" are considered first, since the driver may have model specific icons.
		// An absence of a matching "Prop_ModelNumber_String" then considers the ETrackedDeviceClass ("HMD", "Controller", "GenericTracker", "TrackingReference")
		// since the driver may have specialized icons based on those device class names.
		//
		// An absence of either then falls back to the "system.vrresources" where generic device class icons are then supplied.
		//
		// Please refer to "bin\drivers\uncapped\resources\driver.vrresources" which contains this uncapped configuration.
		//
		// "Alias" is a reserved key and specifies chaining to another json block.
		//
		// In this uncapped configuration file (overly complex FOR EXAMPLE PURPOSES ONLY)....
		//
		// "Model-v2.0" chains through the alias to "Model-v1.0" which chains through the alias to "Model-v Defaults".
		//
		// Keys NOT found in "Model-v2.0" would then chase through the "Alias" to be resolved in "Model-v1.0" and either resolve their or continue through the alias.
		// Thus "Prop_NamedIconPathDeviceAlertLow_String" in each model's block represent a specialization specific for that "model".
		// Keys in "Model-v Defaults" are an example of mapping to the same states, and here all map to "Prop_NamedIconPathDeviceOff_String".
		//
		bool bSetupIconUsingExternalResourceFile = true;
		if ( !bSetupIconUsingExternalResourceFile )
		{
			// Setup properties directly in code.
			// Path values are of the form {drivername}\icons\some_icon_filename.png
			vr::VRProperties()->SetStringProperty( m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceOff_String, "{uncapped}/icons/headset_uncapped_status_off.png" );
			vr::VRProperties()->SetStringProperty( m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceSearching_String, "{uncapped}/icons/headset_uncapped_status_searching.gif" );
			vr::VRProperties()->SetStringProperty( m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceSearchingAlert_String, "{uncapped}/icons/headset_uncapped_status_searching_alert.gif" );
			vr::VRProperties()->SetStringProperty( m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceReady_String, "{uncapped}/icons/headset_uncapped_status_ready.png" );
			vr::VRProperties()->SetStringProperty( m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceReadyAlert_String, "{uncapped}/icons/headset_uncapped_status_ready_alert.png" );
			vr::VRProperties()->SetStringProperty( m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceNotReady_String, "{uncapped}/icons/headset_uncapped_status_error.png" );
			vr::VRProperties()->SetStringProperty( m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceStandby_String, "{uncapped}/icons/headset_uncapped_status_standby.png" );
			vr::VRProperties()->SetStringProperty( m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceAlertLow_String, "{uncapped}/icons/headset_uncapped_status_ready_low.png" );
		}

		return VRInitError_None;
	}

	virtual void Deactivate() 
	{
		m_unObjectId = vr::k_unTrackedDeviceIndexInvalid;
	}

	virtual void EnterStandby()
	{
	}

	void *GetComponent( const char *pchComponentNameAndVersion )
	{
		if ( !_stricmp( pchComponentNameAndVersion, vr::IVRDisplayComponent_Version ) )
		{
			return (vr::IVRDisplayComponent*)this;
		}
		if (!_stricmp(pchComponentNameAndVersion, vr::IVRVirtualDisplay_Version))
		{
			return (vr::IVRVirtualDisplay*)this;
		}
		//if (!_stricmp(pchComponentNameAndVersion, vr::IVRDriverDirectModeComponent_Version))
		//{
		//	return (vr::IVRDriverDirectModeComponent*)this;
		//}

		// override this to add a component to a driver
		return NULL;
	}

	virtual void PowerOff() 
	{
	}

	/** debug request from a client */
	virtual void DebugRequest( const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize ) 
	{
		if( unResponseBufferSize >= 1 )
			pchResponseBuffer[0] = 0;
	}

	virtual void GetWindowBounds( int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight ) 
	{
		*pnX = m_nWindowX;
		*pnY = m_nWindowY;
		*pnWidth = m_nWindowWidth;
		*pnHeight = m_nWindowHeight;
	}

	virtual bool IsDisplayOnDesktop() 
	{
		return false;
	}

	virtual bool IsDisplayRealDisplay() 
	{
		return false;
	}

	virtual void GetRecommendedRenderTargetSize( uint32_t *pnWidth, uint32_t *pnHeight ) 
	{
		*pnWidth = m_nRenderWidth;
		*pnHeight = m_nRenderHeight;
	}

	virtual void GetEyeOutputViewport( EVREye eEye, uint32_t *pnX, uint32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight ) 
	{
		*pnY = 0;
		*pnWidth = m_nWindowWidth / 2;
		*pnHeight = m_nWindowHeight;
	
		if ( eEye == Eye_Left )
		{
			*pnX = 0;
		}
		else
		{
			*pnX = m_nWindowWidth / 2;
		}
	}

	virtual void GetProjectionRaw( EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom ) 
	{
		*pfLeft = -1.0;
		*pfRight = 1.0;
		*pfTop = -1.0;
		*pfBottom = 1.0;	
	}

	virtual DistortionCoordinates_t ComputeDistortion( EVREye eEye, float fU, float fV ) 
	{
		DistortionCoordinates_t coordinates;
		coordinates.rfBlue[0] = fU;
		coordinates.rfBlue[1] = fV;
		coordinates.rfGreen[0] = fU;
		coordinates.rfGreen[1] = fV;
		coordinates.rfRed[0] = fU;
		coordinates.rfRed[1] = fV;
		return coordinates;
	}

#pragma region IVRVirtualDisplay
	virtual void Present(const PresentInfo_t* pPresentInfo, uint32_t unPresentInfoSize)
	{
		//DriverLog("VSync: %d, Time: %f", pPresentInfo->vsync, pPresentInfo->flVSyncTimeInSeconds);
	}

	virtual void WaitForPresent() {
		m_frameCounter++;
	}

	virtual bool GetTimeSinceLastVsync(float* pfSecondsSinceLastVsync, uint64_t* pulFrameCounter) {
		*pfSecondsSinceLastVsync = 0.0;
		*pulFrameCounter = m_frameCounter;
		
		return true;
	}
#pragma endregion

//#pragma region IVRDriverDirectModeComponent
//	/** Called to allocate textures for applications to render into.  One of these per eye will be passed back to SubmitLayer each frame. */
//	virtual void CreateSwapTextureSet(uint32_t unPid, const SwapTextureSetDesc_t* pSwapTextureSetDesc, SwapTextureSet_t* pOutSwapTextureSet) {}
//
//	/** Used to textures created using CreateSwapTextureSet.  Only one of the set's handles needs to be used to destroy the entire set. */
//	virtual void DestroySwapTextureSet(vr::SharedTextureHandle_t sharedTextureHandle) {}
//
//	/** Used to purge all texture sets for a given process. */
//	virtual void DestroyAllSwapTextureSets(uint32_t unPid) {}
//
//	/** After Present returns, calls this to get the next index to use for rendering. */
//	virtual void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t sharedTextureHandles[2], uint32_t(*pIndices)[2]) {}
//
//	virtual void SubmitLayer(const SubmitLayerPerEye_t(&perEye)[2]) {}
//
//	/** Submits queued layers for display. */
//	virtual void Present(vr::SharedTextureHandle_t syncTexture) {}
//
//	/** Called after Present to allow driver to take more time until vsync after they've successfully acquired the sync texture in Present.*/
//	virtual void PostPresent() {}
//
//	/** Called to get additional frame timing stats from driver.  Check m_nSize for versioning (new members will be added to end only). */
//	virtual void GetFrameTiming(DriverDirectMode_FrameTiming* pFrameTiming) {}
//#pragma endregion

	virtual DriverPose_t GetPose() 
	{
		DriverPose_t pose = { 0 };
		pose.poseIsValid = true;
		pose.result = TrackingResult_Running_OK;
		pose.deviceIsConnected = true;

		pose.qWorldFromDriverRotation = HmdQuaternion_Init( 1, 0, 0, 0 );
		pose.qDriverFromHeadRotation = HmdQuaternion_Init( 1, 0, 0, 0 );
		

		return pose;
	}
	

	void RunFrame()
	{
		// In a real driver, this should happen from some pose tracking thread.
		// The RunFrame interval is unspecified and can be very irregular if some other
		// driver blocks it for some periodic task.
		if ( m_unObjectId != vr::k_unTrackedDeviceIndexInvalid )
		{
			vr::VRServerDriverHost()->TrackedDevicePoseUpdated( m_unObjectId, GetPose(), sizeof( DriverPose_t ) );
		}
	}

	std::string GetSerialNumber() const { return m_sSerialNumber; }

private:
	vr::TrackedDeviceIndex_t m_unObjectId;
	vr::PropertyContainerHandle_t m_ulPropertyContainer;

	std::string m_sSerialNumber;
	std::string m_sModelNumber;

	int32_t m_nWindowX;
	int32_t m_nWindowY;
	int32_t m_nWindowWidth;
	int32_t m_nWindowHeight;
	int32_t m_nRenderWidth;
	int32_t m_nRenderHeight;
	float m_flSecondsFromVsyncToPhotons;
	float m_flDisplayFrequency;
	float m_flIPD;

	uint64_t m_frameCounter = 0;
	std::chrono::time_point<std::chrono::high_resolution_clock> lastVsyncTime = std::chrono::high_resolution_clock::now();

	VRInputComponentHandle_t m_proximitySensor;

	CTiming* timingThread = nullptr;
};

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
class CServerDriver_UNCAPPED: public IServerTrackedDeviceProvider
{
public:
	virtual EVRInitError Init( vr::IVRDriverContext *pDriverContext ) ;
	virtual void Cleanup() ;
	virtual const char * const *GetInterfaceVersions() { return vr::k_InterfaceVersions; }
	virtual void RunFrame() ;
	virtual bool ShouldBlockStandbyMode()  { return false; }
	virtual void EnterStandby()  {}
	virtual void LeaveStandby()  {}

private:
	CUNCAPPEDDeviceDriver *m_pNullHmdLatest = nullptr;
};

CServerDriver_UNCAPPED g_serverDriverNull;


EVRInitError CServerDriver_UNCAPPED::Init( vr::IVRDriverContext *pDriverContext )
{
	VR_INIT_SERVER_DRIVER_CONTEXT( pDriverContext );
	InitDriverLog( vr::VRDriverLog() );

	m_pNullHmdLatest = new CUNCAPPEDDeviceDriver();
	vr::VRServerDriverHost()->TrackedDeviceAdded( m_pNullHmdLatest->GetSerialNumber().c_str(), vr::TrackedDeviceClass_HMD, m_pNullHmdLatest );

	return VRInitError_None;
}

void CServerDriver_UNCAPPED::Cleanup() 
{
	CleanupDriverLog();
	delete m_pNullHmdLatest;
	m_pNullHmdLatest = NULL;
}


void CServerDriver_UNCAPPED::RunFrame()
{
	if ( m_pNullHmdLatest )
	{
		m_pNullHmdLatest->RunFrame();
	}
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
HMD_DLL_EXPORT void *HmdDriverFactory( const char *pInterfaceName, int *pReturnCode )
{
	if( 0 == strcmp( IServerTrackedDeviceProvider_Version, pInterfaceName ) )
	{
		return &g_serverDriverNull;
	}
	if( 0 == strcmp( IVRWatchdogProvider_Version, pInterfaceName ) )
	{
		return &g_watchdogDriverNull;
	}

	if( pReturnCode )
		*pReturnCode = VRInitError_Init_InterfaceNotFound;

	return NULL;
}
