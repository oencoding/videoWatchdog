/*
 * freeze.cpp
 *
 *  Created on: Oct 15, 2015
 *      Author: fcaldas
 */

#include "ServerInstance.h"
#include "../recognition/imageRecognition.h"
#include <cpprest/http_listener.h>
#include <cpprest/json.h>
#include <cpprest/http_client.h>
#include <cpprest/astreambuf.h>
#include <cpprest/containerstream.h>
#include <iostream>
#include <string>
#include <sstream>
#include <unistd.h>
#include <sys/time.h>
#include <queue>
#include <pplx/pplxtasks.h>
#include <cpprest/json.h>
#include <system_error>
#include <ctime>
#include <boost/asio.hpp>
#include "tcpClient/tcpClient.h"

using namespace web;
using namespace web::http;
using namespace web::http::experimental::listener;

namespace RestServer {

    /*
     * Detects the state of an STB after a given time of analysis
     */
    void wwwdetectState(http_request request) {
        json::value resp = request.extract_json().get();
        json::value answer;
        if (resp.has_field("timeAnalysis") && resp["timeAnalysis"].is_number()) {
            if (resp["timeAnalysis"].as_integer() > 400) {
                __screenState sState = getState(resp["timeAnalysis"].as_integer());
                answer["MaxDiffpPixel"] = sState.maxDiffppixel;
                answer["NormpPixel"] = sState.maxNormppixel;
                answer["status"] = web::json::value::string(getNameOfState(sState.oState));
            } else {
                answer["error"] = 1;
                answer["message"] = web::json::value::string(
                        "Variable 'timeAnalysis' has to be greater than 400ms");
            }
        } else {
            answer["error"] = 1;
            answer["message"] =
                    web::json::value::string(
                            "This request needs one value: 'timeAnalysis' the time in milliseconds that the video output will be evaluated");
        }
        request.reply(status_codes::OK, answer);
    }

    /*
     * Detects changes of state in a STB for a given time
     */
    void wwwdetectEvent(http_request request) {
        
        json::value params = request.extract_json().get();
        json::value reply;
        
        //check for parameters on received POST request
        if (   params.has_field("timeAnalysis") && params["timeAnalysis"].is_integer()
            && params.has_field("eventType") && params["eventType"].is_array()
            && params.has_field("timeEvent") && params["timeEvent"].is_integer()) {
            
            if (params["timeAnalysis"].as_integer() < 2000) {
                
                reply["error"] = 1;
                reply["message"] = web::json::value::string("'timeAnalysis' needs to be greater than 2000ms");
            } else if (   params["timeEvent"].as_integer() < 500
                       || params["timeEvent"].as_integer() > params["timeAnalysis"].as_integer()) {
                
                reply["error"] = 1;
                std::string message = "'timeEvent' has to be bigger than 500 and smaller than 'timeAnalysis'";
                reply["message"] = web::json::value::string(message);
            } else {
                
                //if everything is ok we then proceed to launch the process
                bool count = false;
                if (   params.has_field("count") 
                    && params["count"].is_boolean()) {
                    count = params["count"].as_bool();
                }
                std::list<outputState> eventsSearch;
                //go through list of events being searched
                
                for (int i = 0; i < params["eventType"].as_array().size(); i++) {
                    outputState oState = getStateByName(params["eventType"].as_array().at(i).as_string());
                    eventsSearch.push_back(oState);
                    if (oState == S_NOT_FOUND) {
                        reply["error"] = 1;
                        std::string message = "Invalid value for eventType, should be: LIVE / FREEZE / BLACK";
                        reply["message"] = web::json::value::string(message);
                        request.reply(status_codes::OK, reply);
                        return;
                    }
                }
                eventsSearch.unique();
                __detectScreenState state = detectStateChange(eventsSearch,
                                                              params["timeAnalysis"].as_integer(),
                                                              params["timeEvent"].as_integer(),
                                                              count);
                //process output
                for (int i = 0; i < state.found.size(); i++) {
                    char buffer[32];
                    json::value tempObj;
    
                    tempObj["foundState"] = web::json::value::string(getNameOfState(state.found[i]));
                    tempObj["observedFor"] = web::json::value::number(state.tlast[i]);
                    struct tm * timeinfo;
                    timeinfo = localtime(&(state.timestamps[i]));
                    std::strftime(buffer, 32, "%d.%m.%Y %H:%M:%S", timeinfo);
                    tempObj["when"] = web::json::value::string(buffer);
                    reply["foundState"][i] = tempObj;
                }
                reply["error"] = 0;
            }
        } else {
            reply["error"] = 1;
            std::string message = "This request needs three parameters: 'timeAnalysis' the time in" \
                                  " milliseconds that the video output will be evaluated, 'eventType': " \
                                  "the type of events we are looking for in an array and 'timeEvent': how long the " \
                                  "event will need to be seen to get an occurrence";
            reply["message"] = web::json::value::string(message);
        }
        request.reply(status_codes::OK, reply);
    }

    /*
     *  Forces a STB to execute a ZAP to any channel and
     *  measures the time it took to execute it.
     *
     */
    void wwwGetZapTime(web::http::http_request request) {
        //check if image field is valid
        json::value params = request.extract_json().get();
        json::value reply;
    
        if (   params.has_field("stb_ip") 
            && params["stb_ip"].is_string() 
            && params.has_field("channel") 
            && params["channel"].is_string()) {
            
            json::value zapRequest, paramsSub;
            zapRequest["action"] = web::json::value::string("zap");
            int chvalue = std::stoi (params["channel"].as_string());
    
            int nDigits;
            if (chvalue < 10) {
                nDigits = 1;
            } else if (chvalue < 100) {
                nDigits = 2;
            } else {
                nDigits = 3;
            }
    
            try {
                
                paramsSub["channelNumber"] = web::json::value::number( chvalue);
                zapRequest["params"] = paramsSub;
                std::string stbAddr = params["stb_ip"].as_string();
                std::string bufferData = zapRequest.serialize();
    
                //boost connect to server and do request
                websocket::tcpClient tClient;
                if (!tClient.conn(stbAddr,8080)) {
                    throw std::runtime_error("Error could not connect to "+stbAddr + ":8080");
                }
                tClient.send_data("POST /message HTTP/1.1\r\n");
                tClient.send_data("Content-Type: application/json\r\n");
                tClient.send_data("Content-Length: " + std::to_string(44 + nDigits) + "\r\n");
                tClient.send_data("Accept: */*\r\n");
                tClient.send_data("Content-Type: application/json\r\n\r\n");
                tClient.send_data(bufferData);
                long time = detectStartAndEndOfBlackScreen(15000);
                tClient.close();
                
                if (time < 0) {
                    reply["error"] = 1;
                    std::string message = "Zap failed, not detected in 15 seconds";
                    reply["message"] = web::json::value::string(message);
                } else {
                    reply["done"] = 1;
                    reply["ms"] = time;
                }
    
            } catch(const std::exception &e) {
                reply["error"] = 1;
                reply["message"] = web::json::value::string(e.what());
            }
        } else {
            reply["error"] = 1;
            std::string message = "This request needs the following parameters: 'stb_ip' address " \
                                  "of the set top box in test, " \
                                  " 'channelNumber':number as string with the channel that we will zap to";
            reply["message"] = web::json::value::string(message);
        }
        request.reply(status_codes::OK, reply);
    }

    /*
     *  Forces a STB to execute a ZAP with raspberry to any channel and
     *  measures the time it took to execute it.
     *
     */
    void wwwRbGetZapTime(web::http::http_request request) {
    
        json::value params = request.extract_json().get();
        json::value reply;
    
        if (   params.has_field("stb_ip") && params["stb_ip"].is_string() 
            && params.has_field("channel") && params["channel"].is_string()) {
            
            json::value zapRequest;
    
            int dt_interDigits = 350; //microseconds
    
            zapRequest["action"]      = web::json::value::string("key");
            zapRequest["params"]      = web::json::value::string("Numeric1");
            zapRequest["outputs"]     = web::json::value::array();
            zapRequest["remoteName"]  = web::json::value::string("REMOTE-G6");
    
            std::string stbAddr = params["stb_ip"].as_string();
            std::string channel = params["channel"].as_string();
                    
            try {
    
                websocket::tcpClient tClient;
                
                if (!tClient.conn(stbAddr,3636)) {
                    throw std::runtime_error("Error could not connect to " + stbAddr + ":3636");
                }
    
                for(std::string::iterator it = channel.begin(); it != channel.end(); it++) {
                    
                    std::stringstream strValue;
                    strValue << *it;
    
                    unsigned int chvalue;
                    strValue >> chvalue;
                    
                    switch (chvalue) {
                        case 0:
                            zapRequest["params"] = web::json::value::string("Numeric0");
                            break;
                        case 1:
                            zapRequest["params"] = web::json::value::string("Numeric1");
                            break;
                        case 2:
                            zapRequest["params"] = web::json::value::string("Numeric2");
                            break;
                        case 3:
                            zapRequest["params"] = web::json::value::string("Numeric3");
                            break;
                        case 4:
                            zapRequest["params"] = web::json::value::string("Numeric4");
                            break;
                        case 5:
                            zapRequest["params"] = web::json::value::string("Numeric5");
                            break;
                        case 6:
                            zapRequest["params"] = web::json::value::string("Numeric6");
                            break;
                        case 7:
                            zapRequest["params"] = web::json::value::string("Numeric7");
                            break;
                        case 8:
                            zapRequest["params"] = web::json::value::string("Numeric8");
                            break;
                        case 9:
                            zapRequest["params"] = web::json::value::string("Numeric9");
                            break;
                    }
                    
                    std::string bufferData = zapRequest.serialize();
    
                    tClient.send_data("POST /message HTTP/1.1\r\n");
                    tClient.send_data("Content-Type: application/json\r\n");
                    tClient.send_data("Content-Length: " + std::to_string(74) + "\r\n");
                    tClient.send_data("Accept: */*\r\n");
                    tClient.send_data("Content-Type: application/json\r\n\r\n");
                    tClient.send_data(bufferData);
                    
                    usleep(1000 * dt_interDigits);
                }
    
                tClient.close();
                
            } catch (const std::exception &e) {
                
                reply["error"]   = 1;
                reply["message"] = web::json::value::string(e.what());
            }
                
            long time = detectStartAndEndOfBlackScreen(15000);
            
            if (time < 0) {
                
                reply["error"] = 1;
                std::string message = "Zap failed, not detected in 15 seconds";
                reply["message"] = web::json::value::string(message);
            } else {
                
                reply["done"] = 1;
                reply["ms"] = time;
            }
            
        } else {
            
            reply["error"] = 1;
            std::string message = "This request needs the following parameters: 'stb_ip' address " \
                                  "of the set top box in test, " \
                                  " 'channelNumber':number as string with the channel that we will zap to";
            reply["message"] = web::json::value::string(message);
        }
        
        request.reply(status_codes::OK, reply);
    }

    /*
     *  Return the state of the screen after an dt_ms long analysis
     *     during this analysis a frame will be captured every dt_interFramems
     */
    __screenState getState(int dt_ms) {
        
        __screenState reply;
        int dt_interFramems = 100; //one shot per 100ms = we'll try to keep 10fps on average
        int nReadings = dt_ms / dt_interFramems;
    
        cv::Mat imgcmp, subResult;
        cv::Mat fimg;
        double maxDiff = 0, diff;
        timeval t0, t1;
    
        try {
            fimg = ServerInstance::cameraDeckLink->captureLastCvMatClone();
        } catch(const CardException &d) {
            
            if(d.getExceptionType() == NO_INPUT_EXCEPTION)
                reply.oState = S_NO_VIDEO;
            return reply;
        }
        bool hasAudio = false;
    
        for (unsigned int i = 0; i < nReadings; i++) {
            gettimeofday(&t0, NULL);
    
            IplImage *pToFree;
            void *ptrAudioData;
            int nBytes;
            imgcmp = ServerInstance::cameraDeckLink->captureLastCvMatAndAudio(&pToFree, &ptrAudioData, &nBytes);
            short *audioData = (short*)ptrAudioData;
            
            for (unsigned int i = 0; i < nBytes / sizeof(short); i++)
                if (std::abs(audioData[i]) > soundThreshold)
                    hasAudio = true;
            free(ptrAudioData);
    
            cv::subtract(imgcmp, fimg, subResult);
            diff = cv::norm(subResult);
            maxDiff = (diff > maxDiff) ? diff : maxDiff;
    
    
            cvRelease((void **) &pToFree);
            gettimeofday(&t1, NULL);
            //fix time: to keep an average of one frame per 50ms
            if (t1.tv_usec - t0.tv_usec + (t1.tv_sec - t0.tv_sec) * 1000000    < 1000 * dt_interFramems)
                usleep(1000 * dt_interFramems - (t1.tv_usec - t0.tv_usec + (t1.tv_sec - t0.tv_sec) * 1000000));
    
        }
    
        if (   maxDiff / (fimg.rows * fimg.cols) < freezeThreshold
            && imageRecognition::isImageBlackScreenOrZapScreen(fimg, blackThreshold)) {
            
            if (hasAudio)
                reply.oState = S_BLACK_SCREEN;
            else
                reply.oState = S_BLACK_SCREEN_NO_AUDIO;
        } else if (maxDiff / (fimg.rows * fimg.cols) < freezeThreshold) {
            
            if(hasAudio)
                reply.oState = S_FREEZE_SIGNAL;
            else
                reply.oState = S_FREEZE_SIGNAL_NO_AUDIO;
        } else {
            
            reply.oState = S_LIVE_SIGNAL;
        }
        
        reply.maxDiffppixel = maxDiff / (fimg.rows * fimg.cols);
        reply.maxNormppixel = cv::norm(fimg) / (fimg.rows * fimg.cols);
        return reply;
    }

    /*
     *  Return if the audio is OK the screen after an dt_ms long analysis
     *     during this analysis a frame will be captured every dt_interFramems
     */
    bool getAudioState(int dt_ms) {
        int dt_interFramems = 100; //one shot per 100ms = we'll try to keep 10fps on average
        int nReadings = dt_ms / dt_interFramems;
    
        cv::Mat imgcmp, subResult;
        cv::Mat fimg;
        timeval t0, t1;
    
        bool hasAudio = false;
    
        for (unsigned int i = 0; i < nReadings; i++) {
            gettimeofday(&t0, NULL);
    
            IplImage *pToFree;
            void *ptrAudioData;
            int nBytes;
            imgcmp = ServerInstance::cameraDeckLink->captureLastCvMatAndAudio(&pToFree, &ptrAudioData, &nBytes);
            short *audioData = (short*)ptrAudioData;
            
            for (unsigned int i = 0; i < nBytes / sizeof(short); i++)
                if(std::abs(audioData[i]) > soundThreshold)
                    hasAudio = true;
            free(ptrAudioData);
    
            cvRelease((void **) &pToFree);
            gettimeofday(&t1, NULL);
            //fix time: to keep an average of one frame per 50ms
            if (t1.tv_usec - t0.tv_usec + (t1.tv_sec - t0.tv_sec) * 1000000    < 1000 * dt_interFramems)
                usleep(1000 * dt_interFramems - (t1.tv_usec - t0.tv_usec + (t1.tv_sec - t0.tv_sec) * 1000000));
    
        }
            return hasAudio ;
    
    }

    /*
     * Detects the start and end of an black screen can be used to measure zapping time
     * by doing a zap via RCU and measuring how long it takes for an black screen to
     * show and disappear.
     */
    
    long detectStartAndEndOfBlackScreen(long maxTimeSearch) {
        
        long time = 0;
        timeval t0, t1;
        gettimeofday(&t0, NULL);
        bool appeared = false;
        while (time < maxTimeSearch) {
            IplImage *imgDt;
            cv::Mat m = ServerInstance::cameraDeckLink->captureLastCvMat(&imgDt);
            if (imageRecognition::isImageBlackScreenOrZapScreen(m,blackThreshold)) {
                if (appeared == false)
                    appeared = true;
            } else {
                if (appeared == true) {
                    gettimeofday(&t1, NULL);
                    time = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec-t0.tv_usec) / 1000;
                    break;
                }
    
            }
            cvRelease((void **) &imgDt);
    
            gettimeofday(&t1, NULL);
            time = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec-t0.tv_usec) / 1000;
    
        }
        
        if (time >= maxTimeSearch)
            return -1;
        return time;
    }

    /*
     * Detect time that a STB takes to have a live signal for 10 seconds
     * this detects when the STB is back from an standy/power off
     *
     * Returns how long the whole process took in milliseconds7
     */
    long detectWakeUP(long maxTimeSearch) {
        
        long time = 0;
        timeval t0, t1;
        gettimeofday(&t0, NULL);
    
        std::deque<__screenState> detectedStates;
    
        while (time < maxTimeSearch) {
            try {
                detectedStates.push_back(getState(1000));
                if (detectedStates.size() > 10) {
                    detectedStates.pop_front();
                    bool isLiveStream = true;
                    
                    for (int i = 0; i < detectedStates.size(); i++) {
                        if(detectedStates[i].oState != S_LIVE_SIGNAL)
                            isLiveStream = false;
                    }
                    
                    if (isLiveStream) {
                        gettimeofday(&t1, NULL);
                        time = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec-t0.tv_usec) / 1000;
                        time -= 1000*10;
                        break;
                    }
                }
            } catch(const std::exception &e) {
                detectedStates.clear();
            }
            //on end
            gettimeofday(&t1, NULL);
            time = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec-t0.tv_usec) / 1000;
    
        }
    
        if (time >= maxTimeSearch)
            return -1;
        return time;
    }

    /*
     * Monitors state changes on screen, this has a limit of around 20 seconds,
     * for anything longer use HDMI watchdog
     */
    __detectScreenState detectStateChange(std::list<outputState>  &stateSearch,
                                          unsigned int timeAnalysis,
                                          unsigned int timeEvent,
                                          bool countOc) {
        
        __detectScreenState screenDetection;
        bool searchBlack  = (std::find(stateSearch.begin(), stateSearch.end(), S_BLACK_SCREEN)  != stateSearch.end());
        bool searchFreeze = (std::find(stateSearch.begin(), stateSearch.end(), S_FREEZE_SIGNAL) != stateSearch.end());
        bool searchLive   = (std::find(stateSearch.begin(), stateSearch.end(), S_LIVE_SIGNAL)   != stateSearch.end());
        outputState lastCapturedState = S_NOT_FOUND;
    
        //number of frames used for status calculation 1 frame / 100ms
        int nFrames = timeEvent / 100;
        if(nFrames > 15)
            nFrames = 15; //saturate otherwise we'll have too many comparisons
    
        int dt_interFramems =  timeEvent / nFrames;
    
        unsigned int nReadings = timeAnalysis / dt_interFramems;
        std::deque<cv::Mat> matList;
        std::deque<IplImage *> toFreeList;
        std::deque<bool> vmatHasAudio;
        timeval t0, t1, tStart;
    
        gettimeofday(&tStart, NULL);
        long culmulativeDelay = 0;
        
        for (unsigned int i = 0; i < nReadings; i++) {
            
            gettimeofday(&t0, NULL);
            try {
                
                void *ptrToAudio;
                int nBytes;
                IplImage* imgIpl;
                matList.push_back(ServerInstance::cameraDeckLink->captureLastCvMatAndAudio(&imgIpl,&ptrToAudio,&nBytes));
                toFreeList.push_back(imgIpl);
                int nSamples = nBytes / sizeof(short);
                short * audioData = (short *) ptrToAudio;
                bool hasAudio = false;
                for (int j = 0; j < nSamples; j++) {
                    if (audioData[j] > soundThreshold)
                        hasAudio = true;
                }
                vmatHasAudio.push_back(hasAudio);
                free(ptrToAudio);
            } catch(const CardException &e) {
                
                usleep(1000 * dt_interFramems);
                std::cout<<"Caught exception on detectState():"<<e.what()<<std::endl<<std::flush;
                continue;
            }
            if (matList.size() > nFrames) {
                
                matList.pop_front();
                cvRelease((void **) &(toFreeList.front()));
                toFreeList.pop_front();
                vmatHasAudio.pop_front();
            }
            //deque is full therefore we can process
            if (matList.size() == nFrames) {
                
                double maxDiff = 0;
                unsigned int npixel = matList[0].cols * matList[0].rows;
                cv::Mat subtractionResult;
                bool framesHaveAudio = false;
                
                for (unsigned int i = 1; i < matList.size(); i++) {
                
                    double n1,n2;
                    cv::subtract(matList[0], matList[i], subtractionResult);
                    n1 = cv::norm(subtractionResult);
                    cv::subtract(matList[i], matList[0], subtractionResult);
                    n2 = cv::norm(subtractionResult);
                    maxDiff = (n1 > maxDiff) ? n1 : maxDiff;
                    maxDiff = (n2 > maxDiff) ? n2 : maxDiff;
                    if (vmatHasAudio[i])
                        framesHaveAudio = true;
                }
                maxDiff = maxDiff / npixel;
    
                if (searchLive && maxDiff > freezeThreshold) {
    
                    if (lastCapturedState == S_LIVE_SIGNAL) {
                        
                        //just increment last capture
                        screenDetection.tlast[screenDetection.tlast.size() - 1] += dt_interFramems;
                    } else {
                        
                        screenDetection.timestamps.push_back(std::time(NULL));
                        screenDetection.found.push_back(S_LIVE_SIGNAL);
                        if(lastCapturedState == S_NOT_FOUND)
                            screenDetection.tlast.push_back(timeEvent);
                        else
                            screenDetection.tlast.push_back(dt_interFramems);
                    }
                    lastCapturedState = S_LIVE_SIGNAL;
                } else if (searchBlack
                        && maxDiff < freezeThreshold
                        && imageRecognition::isImageBlackScreenOrZapScreen(matList[0],blackThreshold)) {
                    
                    if (   lastCapturedState == S_BLACK_SCREEN 
                        || lastCapturedState == S_BLACK_SCREEN_NO_AUDIO) {
                        
                        screenDetection.tlast[screenDetection.tlast.size() - 1] += dt_interFramems;
                    } else {
                        
                        screenDetection.timestamps.push_back(std::time(NULL));
                        if(framesHaveAudio)
                            screenDetection.found.push_back(S_BLACK_SCREEN);
                        else
                            screenDetection.found.push_back(S_BLACK_SCREEN_NO_AUDIO);
   
                        if (lastCapturedState == S_NOT_FOUND)
                            screenDetection.tlast.push_back(timeEvent);
                        else
                            screenDetection.tlast.push_back(dt_interFramems);
                    }
                    lastCapturedState = S_BLACK_SCREEN;
                } else if (   searchFreeze
                           && maxDiff < freezeThreshold
                           && !imageRecognition::isImageBlackScreenOrZapScreen(matList[0],blackThreshold)) {
                    
                    if (   lastCapturedState == S_FREEZE_SIGNAL 
                        || lastCapturedState == S_FREEZE_SIGNAL_NO_AUDIO) {
                        
                        screenDetection.tlast[screenDetection.tlast.size() - 1] += dt_interFramems;
                    } else {
                        
                        screenDetection.timestamps.push_back(std::time(NULL));
    
                        if (framesHaveAudio)
                            screenDetection.found.push_back(S_FREEZE_SIGNAL);
                        else
                            screenDetection.found.push_back(S_FREEZE_SIGNAL_NO_AUDIO);
    
                        if (lastCapturedState == S_NOT_FOUND)
                            screenDetection.tlast.push_back(timeEvent);
                        else
                            screenDetection.tlast.push_back(dt_interFramems);
                    }
                    
                    lastCapturedState = S_FREEZE_SIGNAL;
                } else {
                    
                    lastCapturedState = S_NOT_FOUND;
                }
            }
            gettimeofday(&t1, NULL);
    
            if (countOc == false && screenDetection.found.size() > 0)
                return screenDetection;
    
            if (t1.tv_usec - t0.tv_usec + (t1.tv_sec - t0.tv_sec) * 1000000 + culmulativeDelay < 1000 * dt_interFramems) {
                
                usleep(    1000 * dt_interFramems - (t1.tv_usec - t0.tv_usec + (t1.tv_sec - t0.tv_sec) * 1000000 - culmulativeDelay));
                culmulativeDelay = 0;
            } else {
                
                culmulativeDelay = culmulativeDelay - (1000 * dt_interFramems - (t1.tv_usec - t0.tv_usec + (t1.tv_sec - t0.tv_sec) * 1000000));
            }
        }
        return screenDetection;
    }


    /*
     * Returns an string representing the name of an outputState object
     */
    std::string getNameOfState(outputState o) {
        switch(o) {
            case S_LIVE_SIGNAL:
                return "Live signal";
            case S_FREEZE_SIGNAL:
                return "Freeze";
            case S_NOT_FOUND:
                return "Output not found";
            case S_NO_VIDEO:
                return "No video";
            case S_BLACK_SCREEN:
                return "Black Screen";
            case S_BLACK_SCREEN_NO_AUDIO:
                return "Black Screen and no audio";
            case S_FREEZE_SIGNAL_NO_AUDIO:
                return "Freeze and no audio";
        }
        return "Not found";
    }

    /*
     * Given a name of an outputState object returns its name
     */
    outputState getStateByName(std::string name) {
        if(name == "LIVE")
            return S_LIVE_SIGNAL;
        else if(name == "FREEZE")
            return S_FREEZE_SIGNAL;
        else if(name == "FREEZE_NO_AUDIO")
                return S_FREEZE_SIGNAL_NO_AUDIO;
        else if(name == "BLACK")
            return S_BLACK_SCREEN;
        else if(name == "BLACK_NO_AUDIO")
            return S_BLACK_SCREEN_NO_AUDIO;
        else if(name == "NOSIGNAL")
            return S_NO_VIDEO;
        return S_NOT_FOUND;
    }

};
