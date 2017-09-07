#include "Lepton3.hpp"

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

#include "LEPTON_SDK.h"
#include "LEPTON_SYS.h"
#include "LEPTON_AGC.h"
#include "LEPTON_RAD.h"

#include <bitset>

#define KELVIN (-273.15f)

using namespace std;

Lepton3::Lepton3(std::string spiDevice, uint16_t cciPort, DebugLvl dbgLvl )
	: mThread()
    , mSpiSegmBuf(NULL)
{
    // >>>>> VoSPI
	mSpiDevice = spiDevice;	
    mSpiFd = -1;

    mSpiMode = SPI_MODE_3; // CPOL=1 (Clock Idle high level), CPHA=1 (SDO transmit/change edge idle to active)
    mSpiBits = 8;
    mSpiSpeed = 32000000; // Max available SPI speed (according to Lepton3 datasheet)

    mPacketCount=60; // default no Telemetry
    mPacketSize=164; // default 14 bit raw data
    mSegmentCount=4; // 4 segments for each unique frame

    mSegmentFreq=106.0f; // According to datasheet each segment is ready at 106 Hz
    
    mSpiSegmBufSize=mPacketCount*mPacketSize;
    mSpiSegmBuf = new uint8_t[mSpiSegmBufSize];
    
    //mSpiPackBuf = new uint8_t[mPacketSize];
        
    mSpiTR.tx_buf = (unsigned long)NULL;
    mSpiTR.delay_usecs = 50;
    mSpiTR.speed_hz = mSpiSpeed;
    mSpiTR.bits_per_word = mSpiBits;
	mSpiTR.cs_change = 0;
	mSpiTR.tx_nbits = 0;
    mSpiTR.rx_nbits = 0;
    mSpiTR.pad = 0;   
    // <<<<< VoSPI

    // >>>>> CCI
    mCciPort = cciPort;
    // <<<<< CCI

    mDebugLvl = dbgLvl;

    if( mDebugLvl>=DBG_INFO )
        cout << "Debug level: " << mDebugLvl << endl;

	mStop = false;
}

Lepton3::~Lepton3()
{
	stop();

    if(mSpiSegmBuf)
        delete [] mSpiSegmBuf;
}

bool Lepton3::start()
{
    mThread = std::thread( &Lepton3::thread_func, this );    
}

void Lepton3::stop()
{
	mStop = true;

    if(mThread.joinable())
    {
        mThread.join();
    }
}

bool Lepton3::SpiOpenPort( )
{
    int status_value = -1;

    if( mDebugLvl>=DBG_INFO )
        cout << "Opening SPI device: " << mSpiDevice.c_str() << endl;

    mSpiFd = open(mSpiDevice.c_str(), O_RDWR);

    if(mSpiFd < 0)
    {
        cerr << "Error - Could not open SPI device: " << mSpiDevice.c_str() << endl;
        return false;
    }

    status_value = ioctl(mSpiFd, SPI_IOC_WR_MODE, &mSpiMode);
    if(status_value < 0)
    {
        cerr << "Could not set SPIMode (WR)...ioctl fail" << endl;
        return false;
    }

    status_value = ioctl(mSpiFd, SPI_IOC_RD_MODE, &mSpiMode);
    if(status_value < 0)
    {
        cerr << "Could not set SPIMode (RD)...ioctl fail" << endl;
        return -1;
    }

    if( mDebugLvl>=DBG_INFO )
        cout << "SPI mode: " << (int)mSpiMode << endl;

    status_value = ioctl(mSpiFd, SPI_IOC_WR_BITS_PER_WORD, &mSpiBits);
    if(status_value < 0)
    {
        cerr << "Could not set SPI bitsPerWord (WR)...ioctl fail" << endl;
        return false;
    }

    status_value = ioctl(mSpiFd, SPI_IOC_RD_BITS_PER_WORD, &mSpiBits);
    if(status_value < 0)
    {
        cerr << "Could not set SPI bitsPerWord(RD)...ioctl fail" << endl;
        return false;
    }

    if( mDebugLvl>=DBG_INFO )
        cout << "SPI bits per word: " << (int)mSpiBits << endl;

    status_value = ioctl(mSpiFd, SPI_IOC_WR_MAX_SPEED_HZ, &mSpiSpeed);
    if(status_value < 0)
    {
        cerr << "Could not set SPI speed (WR)...ioctl fail" << endl;
        return false;
    }

    status_value = ioctl(mSpiFd, SPI_IOC_RD_MAX_SPEED_HZ, &mSpiSpeed);
    if(status_value < 0)
    {
        cerr << "Could not set SPI speed (RD)...ioctl fail" << endl;
        return false;
    }

    if( mDebugLvl>=DBG_INFO )
        cout << "SPI max speed: " << (int)mSpiSpeed << endl;

    return true;
}

void Lepton3::SpiClosePort()
{
    if( mSpiFd<0 )
        return;

    int status_value = close(mSpiFd);
    if(status_value < 0)
    {
        cerr << "Error closing SPI device [" << mSpiFd << "] " << mSpiDevice;
    }
}

int Lepton3::SpiReadSegment()
{
    if( mSpiFd<0 )
    {
        if( mDebugLvl>=DBG_FULL )
        {
            cout << "SPI device not open. Trying to open it..." << endl;
        }
        if( !SpiOpenPort() )
            return -1;
    }
    
    // >>>>> Wait first valid packet
    mSpiTR.cs_change = 0;
    mSpiTR.rx_buf = (unsigned long)(mSpiSegmBuf); // First Packet has been read above
    mSpiTR.len = mPacketSize;
    while(1)
    {
    	if( mStop )
    	{
    		return -1;
    	}
    	
    	int ret = ioctl( mSpiFd, SPI_IOC_MESSAGE(1), &mSpiTR );
	    if (ret == 1)
	    {
	        cerr << "Error reading full segment from SPI" << endl;
            return -1;
	    }
    	
    	if( (mSpiSegmBuf[0] & 0x0f) == 0x0f ) // Packet not valid
    		continue;
    	
    	if( mSpiSegmBuf[1] == 0 ) // First valid packet
    		break;    	
    }
    // <<<<< Wait first valid packet */

    // >>>>> Segment reading
    mSpiTR.rx_buf = (unsigned long)(mSpiSegmBuf+mPacketSize); // First Packet has been read above
    mSpiTR.len = mSpiSegmBufSize-mPacketSize;
	mSpiTR.cs_change = 0;
    
    int ret = ioctl( mSpiFd, SPI_IOC_MESSAGE(1), &mSpiTR );
	if (ret == 1)
	{
	    cerr << "Error reading full segment from SPI" << endl;
        return -1;
	}
    // <<<<< Segment reading 

    // >>>>> Segment ID
    // Segment ID is written in the 21th Packet int the bit 1-3 of the first byte (the first bit is always 0)
    // Packet number is written in the bit 4-7 of the first byte

    uint8_t pktNumber = mSpiSegmBuf[20*mPacketSize+1];
    
    if( mDebugLvl>=DBG_FULL )
    {
        cout << "{" << (int)pktNumber << "} ";
    }
    
    if( pktNumber!=20 )
    {
        if( mDebugLvl>=DBG_INFO )
        {
            cout << "Wrong Packet ID for TTT in segment" << endl;
            return -1;
        }
    }       

    int segmentID = (mSpiSegmBuf[20*mPacketSize] & 0x70) >> 4;
    // <<<<< Segment ID

    return segmentID;
}

void Lepton3::thread_func()
{
    if( mDebugLvl>=DBG_INFO )
        cout << "Grabber thread started ..." << endl;
	
    mStop = false;
	
    int ret = 0;
	
    if( !SpiOpenPort() )
    {
        cerr << "Grabber thread stopped on starting for SPI error" << endl;
        return;
    }
	
    if( mDebugLvl>=DBG_FULL )
        cout << "SPI fd: " << mSpiFd << endl;
        
    int notValidCount = 0;
    int nextSegment = 1;

	while(true) 
	{
		double elapsed = mThreadWatch.toc();
		mThreadWatch.tic();
		
		int toWait = (int)((1/mSegmentFreq)*1000*1000)-elapsed;
		
		cout << endl << "Elapsed " << elapsed << " usec - Available: " << toWait << endl << endl;	
		
		int segment = SpiReadSegment();        
	    
    	if( segment!=-1 )
    	{
    	    if( mDebugLvl>=DBG_FULL )
    	    {
    	        cout << "Retrieved segment: " << segment;
    	    }
    	    
    	    if( segment != 0 )
    	    {
    	        if( mDebugLvl>=DBG_FULL )
    	        {
                    cout << " <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<";
                }
                
    	        notValidCount=0;
    	        
    	        if(segment==nextSegment)
    	        {
    	            nextSegment++;
    	        }
    	        
    	        if(nextSegment==5)
    	        {
    	            // FRAME COMPLETE
    	            
    	            if( mDebugLvl>=DBG_FULL )
        	        {
                        cout << endl << "************************ FRAME COMPLETE ************************" << endl;
                    }
    	        }
    	    }
        	else
            {  
                nextSegment=1;
                notValidCount++;
            }
            
            if( mDebugLvl>=DBG_FULL )
    	    {
                cout << endl; 
            }
        }
        else
        {
            notValidCount++;
        }
        
        if( notValidCount>10 )
        {
            resync();
            
            notValidCount=0;
        }
	    
	    //SpiReadPacket();
	    
	    if( mStop )
	    {
	    	if( mDebugLvl>=DBG_INFO )
        		cout << "... grabber thread stopped ..." << endl;
        		
        	break;
	    }  
	    
	    //usleep(10000);
	    /*if(toWait>0)
	    {
	    	std::this_thread::sleep_for(std::chrono::microseconds(toWait));
	    }//*/
	    
	    /*SpiClosePort();
	    std::this_thread::sleep_for(std::chrono::microseconds(175000));
	    SpiOpenPort();*/
	}
	
	//finally, close SPI port just bcuz
    SpiClosePort();
	
    if( mDebugLvl>=DBG_INFO )
        cout << "... grabber thread finished" << endl;
}

void Lepton3::resync()
{
    if( mDebugLvl>=DBG_INFO )
    {
        cout << endl << "!!!!!!!!!!!!!!!!!!!! RESYNC !!!!!!!!!!!!!!!!!!!!" << endl;
    }
        
    // >>>>> Resync
    uint8_t dummyBuf[5];
    mSpiTR.rx_buf = (unsigned long)(dummyBuf); // First Packet has been read above
    mSpiTR.len = 5;
    mSpiTR.cs_change = 1; // Force deselect after "ioctl"

    ioctl( mSpiFd, SPI_IOC_MESSAGE(1), &mSpiTR );     
    
    // Keeps /CS High for 185 msec according to datasheet
    std::this_thread::sleep_for(std::chrono::microseconds(185000));  
    // <<<<< Resync
}

bool Lepton3::CciConnect()
{
    int result = LEP_OpenPort( mCciPort, LEP_CCI_TWI, 400, mCciConnPort );

    if (result != LEP_OK)
    {
        cerr << "Cannot connect CCI port (I2C)" << endl;
        return false;
    }

    mCciConnected = true;
    return true;
}

float Lepton3::getSensorTemperatureK()
{
    if(!mCciConnected)
    {
        if( !CciConnect() )
            return KELVIN;
    }

    LEP_SYS_FPA_TEMPERATURE_KELVIN_T temp;

    LEP_RESULT result = LEP_GetSysFpaTemperatureKelvin( mCciConnPort, (LEP_SYS_FPA_TEMPERATURE_KELVIN_T_PTR)(&temp));

    if (result != LEP_OK)
    {
        cerr << "Cannot read lepton FPA temperature" << endl;
        return false;
    }

    float tempK = (float)(temp)/100.0f;

    if( mDebugLvl>=DBG_INFO )
        cout << "FPA temperature: " << tempK << "°K - " ;

    return tempK;
}


float Lepton3::raw2Celsius(float raw)
{
    float ambientTemperature = 25.0;
    float slope = 0.0217;

    return slope*raw+ambientTemperature-177.77;
}

bool Lepton3::lepton_perform_ffc()
{
    if(!mCciConnected)
    {
        if( !CciConnect() )
            return false;
    }

    if( LEP_RunSysFFCNormalization(mCciConnPort) != LEP_OK )
    {
    	cerr << "Could not perform FFC Normalization" << endl;
    	return false;
    }
}

int Lepton3::enableRadiometry( bool enable )
{
    if(!mCciConnected)
    {
        CciConnect();
    }

    LEP_RAD_ENABLE_E rad_status;

    if( LEP_GetRadEnableState(mCciConnPort, (LEP_RAD_ENABLE_E_PTR)&rad_status ) != LEP_OK )
        return -1;

    LEP_RAD_ENABLE_E new_status = enable?LEP_RAD_ENABLE:LEP_RAD_DISABLE;

    if( rad_status != new_status )
    {
        if( LEP_SetRadEnableState(mCciConnPort, new_status ) != LEP_OK )
            return -1;
    }

    return new_status;
}
