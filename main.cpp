/*
 * Copyright (c) 2017 ARM Limited. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mbed.h"
#include "common_functions.h"
#include "CellularNonIPSocket.h"
#include "CellularDevice.h"
#include "UDPSocket.h"
#include "CellularLog.h"
#include <MQTTClientMbedOs.h>  
#include "MAX30001.h"

#define RTOR1 0x3fa300
#define UDP 0
#define TCP 1
#define NONIP 2

char message_buffer[256];  
float BPM;
uint32_t RtoR;

/* ECG FIFO nearly full callback */
volatile bool ecgIntFlag = 0; 
void ecgFIFO_callback()  {
    
    ecgIntFlag = 1;
} 

// Number of retries /
#define RETRY_COUNT 3

NetworkInterface *iface;

const char *host_name = MBED_CONF_APP_MQTT_BROKER_HOSTNAME;
const int port = MBED_CONF_APP_MQTT_BROKER_PORT;

static rtos::Mutex trace_mutex;

#if MBED_CONF_MBED_TRACE_ENABLE
static void trace_wait()
{
    trace_mutex.lock();
}

static void trace_release()
{
    trace_mutex.unlock();
}

static char time_st[50];

static char* trace_time(size_t ss)
{
    snprintf(time_st, 49, "[%08llums]", Kernel::get_ms_count());
    return time_st;
}
/***********************************************************************************/
static void trace_open()
{
    mbed_trace_init();
    mbed_trace_prefix_function_set( &trace_time );

    mbed_trace_mutex_wait_function_set(trace_wait);
    mbed_trace_mutex_release_function_set(trace_release);

    mbed_cellular_trace::mutex_wait_function_set(trace_wait);
    mbed_cellular_trace::mutex_release_function_set(trace_release);
}
/************************************************************************************/
static void trace_close()
{
    mbed_cellular_trace::mutex_wait_function_set(NULL);
    mbed_cellular_trace::mutex_release_function_set(NULL);

    mbed_trace_free();
}
#endif // #if MBED_CONF_MBED_TRACE_ENABLE
/**************************************************************************************/
Thread dot_thread(osPriorityNormal, 512);

void print_function(const char *format, ...)
{
    trace_mutex.lock();
    va_list arglist;
    va_start( arglist, format );
    vprintf(format, arglist);
    va_end( arglist );
    trace_mutex.unlock();
}
/***************************************************************************************/
void dot_event()
{
    while (true) {
        ThisThread::sleep_for(4000);
        if (iface && iface->get_connection_status() == NSAPI_STATUS_GLOBAL_UP) {
            break;
        } else {
            trace_mutex.lock();
            printf(".");
            fflush(stdout);
            trace_mutex.unlock();
        }
    }
}
/****************************************************************************************/
/**
 * Connects to the Cellular Network
 */
nsapi_error_t do_connect()
{
    nsapi_error_t retcode = NSAPI_ERROR_OK;
    uint8_t retry_counter = 0;

    while (iface->get_connection_status() != NSAPI_STATUS_GLOBAL_UP) {
        retcode = iface->connect();
        if (retcode == NSAPI_ERROR_AUTH_FAILURE) {
            print_function("\n\nAuthentication Failure. Exiting application\n");
        } else if (retcode == NSAPI_ERROR_OK) {
            print_function("\n\nConnection Established.\n");
        } else if (retry_counter > RETRY_COUNT) {
            print_function("\n\nFatal connection failure: %d\n", retcode);
        } else {
            print_function("\n\nCouldn't connect: %d, will retry\n", retcode);
            retry_counter++;
            continue;
        }
        break;
    }
    return retcode;
}

/******************************************************************************/
//TCP SOCKET OPEN AND MQTT CONNECTION
nsapi_error_t sock_open()
{
    nsapi_size_or_error_t retcode;
    TCPSocket sock;
    retcode = sock.open(iface);
    if (retcode != NSAPI_ERROR_OK) {

        print_function("TCPSocket.open() fails, code: %d\n", retcode);

        return -1;
    }
    
   SocketAddress MQTTBroker;
    retcode = iface->gethostbyname(host_name, &MQTTBroker);
    if (retcode != NSAPI_ERROR_OK) {
        print_function("Couldn't resolve remote host: %s, code: %d\n", host_name, retcode);
        return -1;
    }

    MQTTBroker.set_port(port);


    retcode = sock.connect(MQTTBroker);
    if (retcode < 0) {
        print_function("TCPSocket.connect() fails, code: %d\n", retcode);
        return -1;
    } 
    else{
        printf("Connecting to %s:%d\r\n", host_name, port);

    
    }
 
 
 /*****************************************************************************/
    
    MQTTClient client(&sock);
    MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
    data.MQTTVersion = 3;
    data.clientID.cstring = MBED_CONF_APP_MQTT_CLIENTID;
    data.username.cstring = MBED_CONF_APP_MQTT_USERNAME;
    data.password.cstring = MBED_CONF_APP_MQTT_PASSWORD;
   
    printf("MQTT connect ...\r\n");
    retcode = client.connect(data);
    if(retcode!=0){
    printf("MQTT connect Failed\r\n");
    return -1;
    }
    
    printf("Success\r\n");
    
     MQTT::Message message;
        message.qos = MQTT::QOS0;
        message.retained = false;
        message.dup = false;
        

        
        sprintf(message_buffer,"[{\"variable\":\"HR\",\"value\":%f}, {\"variable\":\"rtor\",\"value\":%d}]" , BPM, RtoR);
    
        message.payload = (void *)message_buffer;
        message.payloadlen = strlen(message_buffer);
    
        printf("Publishing HR: %3.2f RtoR: %d\r\n", BPM, RtoR);
        client.publish(MBED_CONF_APP_MQTT_PUBTOPIC, message);

        sock.close();
        return 0;
}

/************************************************************************************/

int main()
{
       //PIN CONFIGURATION
DigitalOut cs(D10);
/// SPI Master 2 with SPI0_SS for use with MAX30001
SPI spi(D11, D12, D13); // used by MAX30001

/// ECG device
MAX30001 max30001(&spi, &cs);
InterruptIn max30001_InterruptB(D5);

    print_function("\n\nmbed-os-example-cellular\n");
    print_function("\n\nBuilt: %s, %s\n", __DATE__, __TIME__);
#ifdef MBED_CONF_NSAPI_DEFAULT_CELLULAR_PLMN
    print_function("\n\n[MAIN], plmn: %s\n", (MBED_CONF_NSAPI_DEFAULT_CELLULAR_PLMN ? MBED_CONF_NSAPI_DEFAULT_CELLULAR_PLMN : "NULL"));
#endif

    print_function("Establishing connection\n");
#if MBED_CONF_MBED_TRACE_ENABLE
    trace_open();
#else
    dot_thread.start(dot_event);
#endif // #if MBED_CONF_MBED_TRACE_ENABLE


    iface = CellularContext::get_default_instance();

    MBED_ASSERT(iface);

    // sim pin, apn, credentials and possible plmn are taken automatically from json when using NetworkInterface::set_default_parameters()
    iface->set_default_parameters();
   
 nsapi_error_t retcode = NSAPI_ERROR_NO_CONNECTION;

//******************************************************************************
  // MAX30001 CONFIG ECG
  //
    uint32_t val;
    uint32_t id;
    int part_version;

    printf("Init ECG\r\n");

    /* the example app specifically states the id has to be read twice */
    max30001.max30001_reg_read(MAX30001::INFO, &id);
    max30001.max30001_reg_read(MAX30001::INFO, &id);

    part_version = (id >> 12) & 0x3;
    if (part_version == 0) {
        printf("Device: MAX30004\r\n");
    } else if (part_version == 1) {
        printf("Device: MAX30001\r\n");
    } else if (part_version == 2) {
        printf("Device: MAX30002\r\n");
    } else if (part_version == 3) {
        printf("Device: MAX30003\r\n");
    }

    max30001_InterruptB.disable_irq();
    max30001_InterruptB.mode(PullUp);
    max30001_InterruptB.fall(&ecgFIFO_callback);
    max30001_InterruptB.enable_irq();

    max30001.max30001_sw_rst();     // Do a software reset of the MAX30001

    max30001.max30001_reg_write(MAX30001::CNFG_EMUX, 0x0);
    max30001.max30001_reg_write(MAX30001::CNFG_GEN, 0x80004);

    max30001.max30001_reg_read(MAX30001::CNFG_RTOR1, &val);
    max30001.max30001_reg_read(MAX30001::CNFG_RTOR1, &val);
    if (RTOR1 != val) {
        max30001.max30001_reg_write(MAX30001::CNFG_RTOR1, RTOR1);
    }
    
 /*********************************************************************************/
 //ECG STARTING
     uint32_t all;

    max30001.max30001_reg_write(MAX30001::EN_INT, 3);
    max30001.max30001_reg_write(MAX30001::EN_INT2, 3);
    max30001.max30001_ECG_InitStart(0x01, 0x00, 0x00, 0x00, 0x02, 0x02,
                           0x05, 0x02, 0x03, 0x01, 0x01);
    max30001.max30001_RtoR_InitStart(0x01, 0x03, 0x0f, 0x02, 0x03,
                            0x20, 0x02, 0x04, 0x01);
    max30001.max30001_INT_assignment(
        MAX30001::MAX30001_INT_B,  // en_enint_loc
        MAX30001::MAX30001_NO_INT, // en_eovf_loc
        MAX30001::MAX30001_NO_INT, // en_fstint_loc

        MAX30001::MAX30001_INT_2B, // en_dcloffint_loc
        MAX30001::MAX30001_INT_B, // en_bint_loc
        MAX30001::MAX30001_NO_INT, // en_bovf_loc

        MAX30001::MAX30001_INT_2B, // en_bover_loc
        MAX30001::MAX30001_INT_2B, // en_bundr_loc
        MAX30001::MAX30001_NO_INT, // en_bcgmon_loc

        MAX30001::MAX30001_INT_B,  // en_pint_loc
        MAX30001::MAX30001_NO_INT, // en_povf_loc,
        MAX30001::MAX30001_NO_INT, // en_pedge_loc

        MAX30001::MAX30001_INT_2B, // en_lonint_loc
        MAX30001::MAX30001_INT_B,  // en_rrint_loc
        MAX30001::MAX30001_NO_INT, //  en_samp_loc

        MAX30001::MAX30001_INT_ODNR,  // intb_Type
        MAX30001::MAX30001_INT_ODNR); // int2b_Type
    //fifo_clear();
    max30001.max30001_synch();
    max30001.max30001_reg_read(MAX30001::STATUS, &all);
    
/*********************************************************************/
//SAMPLES READING



    uint32_t ecgFIFO, readECGSamples,  ETAG[32], status;
    int16_t ecgSample[32];
    
    const int EINT_STATUS =  1 << 23;
    const int RTOR_STATUS =  1 << 10;
    const int RTOR_REG_OFFSET = 10;
    const float RTOR_LSB_RES = 0.0078125f;
    const int FIFO_OVF =  0x7;
    const int FIFO_VALID_SAMPLE =  0x0;
    const int FIFO_FAST_SAMPLE =  0x1;
    const int ETAG_BITS = 0x7;
    
while (1) 
  {
    
        /* Read back ECG samples from the FIFO */
        if( ecgIntFlag ) {
            
            ecgIntFlag = 0;
           
            max30001.max30001_reg_read( MAX30001::STATUS, &status );      // Read the STATUS register

               
            // Check if R-to-R interrupt asserted
            if( ( status & RTOR_STATUS ) == RTOR_STATUS ){           
            
                
                // Read RtoR register
                max30001.max30001_reg_read( MAX30001::RTOR, &RtoR );   
                RtoR = RtoR  >>  RTOR_REG_OFFSET;
                
                // Convert to BPM
                BPM = 1.0f / ( RtoR * RTOR_LSB_RES / 60.0f );   
                
                // Print RtoR              
                printf("RtoR : %d BPM: %3.2f\r\n\r\n", RtoR, BPM);                   
                
            }  
                         // Check if EINT interrupt asserted
            if ( ( status & EINT_STATUS ) == EINT_STATUS ) {     
            
               // debug.printf("FIFO Interrupt \r\n");
                readECGSamples = 0;                        // Reset sample counter
                
                do {
                    max30001.max30001_reg_read( MAX30001::ECG_FIFO, &ecgFIFO );       // Read FIFO
                    ecgSample[readECGSamples] = ecgFIFO >> 8;                  // Isolate voltage data
                    ETAG[readECGSamples] = ( ecgFIFO >> 3 ) & ETAG_BITS;  // Isolate ETAG
                    readECGSamples++;                                          // Increment sample counter
                
                // Check that sample is not last sample in FIFO                                              
                } while ( ETAG[readECGSamples-1] == FIFO_VALID_SAMPLE || 
                          ETAG[readECGSamples-1] == FIFO_FAST_SAMPLE ); 
                
                // Check if FIFO has overflowed
                if( ETAG[readECGSamples - 1] == FIFO_OVF ){                  
                    max30001.max30001_reg_write( MAX30001::FIFO_RST , 0); // Reset FIFO
                   
                }
                           
            }
             //trasmetto il campione solo se la connessione alla rete è andata a buon fine
             // e se i valori di battito non sono troppo bassi o troppo alti
           if (do_connect() == NSAPI_ERROR_OK && BPM < 140 && BPM > 30) {
            retcode = sock_open();
            }

        }
        printf("Waiting...\r\n");    //pausa di 5 secondi
        ThisThread::sleep_for(5000);

    } 
    
/******************************************************************************/
//DISCONNECT

 if (iface->disconnect() != NSAPI_ERROR_OK) {
        print_function("\n\n disconnect failed.\n\n");
    }

    if (retcode == 0) {
        print_function("\n\nSuccess. Exiting \n\n");
    } else {
        print_function("\n\nFailure. Exiting \n\n");
    }
#if MBED_CONF_MBED_TRACE_ENABLE
    trace_close();
#else
    dot_thread.terminate();
#endif // #if MBED_CONF_MBED_TRACE_ENABLE

    return 0;
}
// EOF 
