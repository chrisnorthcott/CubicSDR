#include "SDRPostThread.h"
#include "CubicSDRDefs.h"
#include "CubicSDR.h"

#include <vector>
#include <deque>

SDRPostThread::SDRPostThread() :
        iqDataInQueue(NULL), iqDataOutQueue(NULL), iqVisualQueue(NULL), terminated(false), dcFilter(NULL), num_vis_samples(16384*2), swapIQ(false) {

    // create a lookup table
    for (unsigned int i = 0; i <= 0xffff; i++) {
        liquid_float_complex tmp,tmp_swap;
# if (__BYTE_ORDER == __LITTLE_ENDIAN)
        tmp_swap.imag = tmp.real = (float(i & 0xff) - 127.4f) * (1.0f/128.0f);
        tmp_swap.real = tmp.imag = (float(i >> 8) - 127.4f) * (1.0f/128.0f);
        _lut.push_back(tmp);
        _lut_swap.push_back(tmp_swap);
#else // BIG_ENDIAN
        tmp_swap.imag = tmp.real = (float(i >> 8) - 127.4f) * (1.0f/128.0f);
        tmp_swap.real = tmp.imag = (float(i & 0xff) - 127.4f) * (1.0f/128.0f);
        _lut.push_back(tmp);
        _lut_swap.push_back(tmp_swap);
#endif
    }
}

SDRPostThread::~SDRPostThread() {
}

void SDRPostThread::bindDemodulator(DemodulatorInstance *demod) {
    busy_demod.lock();
    demodulators.push_back(demod);
    busy_demod.unlock();
}

void SDRPostThread::removeDemodulator(DemodulatorInstance *demod) {
    if (!demod) {
        return;
    }

    busy_demod.lock();
    std::vector<DemodulatorInstance *>::iterator i = std::find(demodulators.begin(), demodulators.end(), demod);
    
    if (i != demodulators.end()) {
        demodulators.erase(i);
    }
    busy_demod.unlock();
}

void SDRPostThread::setIQDataInQueue(SDRThreadIQDataQueue* iqDataQueue) {
    iqDataInQueue = iqDataQueue;
}
void SDRPostThread::setIQDataOutQueue(DemodulatorThreadInputQueue* iqDataQueue) {
    iqDataOutQueue = iqDataQueue;
}
void SDRPostThread::setIQVisualQueue(DemodulatorThreadInputQueue *iqVisQueue) {
    iqVisualQueue = iqVisQueue;
}

void SDRPostThread::setNumVisSamples(int num_vis_samples_in) {
    num_vis_samples = num_vis_samples_in;
}

int SDRPostThread::getNumVisSamples() {
    return num_vis_samples;
}

void SDRPostThread::setSwapIQ(bool swapIQ) {
    this->swapIQ.store(swapIQ);
}

bool SDRPostThread::getSwapIQ() {
    return this->swapIQ.load();
}

void SDRPostThread::threadMain() {
    int n_read;
    double seconds = 0.0;

#ifdef __APPLE__
    pthread_t tID = pthread_self();  // ID of this thread
    int priority = sched_get_priority_max( SCHED_FIFO) - 1;
    sched_param prio = {priority}; // scheduling priority of thread
    pthread_setschedparam(tID, SCHED_FIFO, &prio);
#endif

    dcFilter = iirfilt_crcf_create_dc_blocker(0.0005);

    DemodulatorThreadIQData *visualDataOut = new DemodulatorThreadIQData;

    std::cout << "SDR post-processing thread started.." << std::endl;

    std::deque<DemodulatorThreadIQData *> buffers;
    std::deque<DemodulatorThreadIQData *>::iterator buffers_i;
    std::vector<liquid_float_complex> fpData;
    std::vector<liquid_float_complex> dataOut;

    while (!terminated) {
        SDRThreadIQData *data_in;

        iqDataInQueue.load()->pop(data_in);
//        std::lock_guard < std::mutex > lock(data_in->m_mutex);

        if (data_in && data_in->data.size()) {
            int dataSize = data_in->data.size()/2;
            if (dataSize > fpData.capacity()) {
                fpData.reserve(dataSize);
                dataOut.reserve(dataSize);
            }
            if (dataSize != fpData.size()) {
                fpData.resize(dataSize);
                dataOut.resize(dataSize);
            }

            if (swapIQ) {
                for (int i = 0; i < dataSize; i++) {
                    fpData[i] = _lut_swap[*((uint16_t*)&data_in->data[2*i])];
                }
            } else {
                for (int i = 0; i < dataSize; i++) {
                    fpData[i] = _lut[*((uint16_t*)&data_in->data[2*i])];
                }
            }

            iirfilt_crcf_execute_block(dcFilter, &fpData[0], dataSize, &dataOut[0]);

            if (iqDataOutQueue.load() != NULL) {
                DemodulatorThreadIQData *pipeDataOut = new DemodulatorThreadIQData;

                pipeDataOut->frequency = data_in->frequency;
                pipeDataOut->sampleRate = data_in->sampleRate;
                pipeDataOut->data.assign(dataOut.begin(), dataOut.end());
                iqDataOutQueue.load()->push(pipeDataOut);
            }

            if (iqVisualQueue.load() != NULL && iqVisualQueue.load()->empty()) {

                visualDataOut->busy_rw.lock();

                if (visualDataOut->data.size() < num_vis_samples) {
                    if (visualDataOut->data.capacity() < num_vis_samples) {
                        visualDataOut->data.reserve(num_vis_samples);
                    }
                    visualDataOut->data.resize(num_vis_samples);
                }

                visualDataOut->frequency = data_in->frequency;
                visualDataOut->sampleRate = data_in->sampleRate;
                visualDataOut->data.assign(dataOut.begin(), dataOut.begin() + num_vis_samples);

                iqVisualQueue.load()->push(visualDataOut);

                visualDataOut->busy_rw.unlock();
            }
            
            busy_demod.lock();

            int activeDemods = 0;
            bool pushedData = false;

            if (demodulators.size()) {

                std::vector<DemodulatorInstance *>::iterator i;
                for (i = demodulators.begin(); i != demodulators.end(); i++) {
                    DemodulatorInstance *demod = *i;
                    if (demod->getFrequency() != data_in->frequency
                            && abs(data_in->frequency - demod->getFrequency()) > (wxGetApp().getSampleRate() / 2)) {
                        continue;
                    }
                    activeDemods++;
                }

                if (demodulators.size()) {

                    DemodulatorThreadIQData *demodDataOut = NULL;

                    for (buffers_i = buffers.begin(); buffers_i != buffers.end(); buffers_i++) {
                        if ((*buffers_i)->getRefCount() <= 0) {
                            demodDataOut = (*buffers_i);
                            break;
                        }
                    }

                    if (demodDataOut == NULL) {
                        demodDataOut = new DemodulatorThreadIQData;
                        buffers.push_back(demodDataOut);
                    }

//                    std::lock_guard < std::mutex > lock(demodDataOut->m_mutex);
                    demodDataOut->frequency = data_in->frequency;
                    demodDataOut->sampleRate = data_in->sampleRate;
                    demodDataOut->setRefCount(activeDemods);
                    demodDataOut->data.assign(dataOut.begin(), dataOut.end());

                    std::vector<DemodulatorInstance *>::iterator i;
                    for (i = demodulators.begin(); i != demodulators.end(); i++) {
                        DemodulatorInstance *demod = *i;
                        DemodulatorThreadInputQueue *demodQueue = demod->threadQueueDemod;

                        if (abs(data_in->frequency - demod->getFrequency()) > (wxGetApp().getSampleRate() / 2)) {
                            if (demod->isActive() && !demod->isFollow() && !demod->isTracking()) {
                                demod->setActive(false);
                                DemodulatorThreadIQData *dummyDataOut = new DemodulatorThreadIQData;
                                dummyDataOut->frequency = data_in->frequency;
                                dummyDataOut->sampleRate = data_in->sampleRate;
                                demodQueue->push(dummyDataOut);
                            }

                            if (demod->isFollow() && wxGetApp().getFrequency() != demod->getFrequency()) {
                                wxGetApp().setFrequency(demod->getFrequency());
                            }
                        } else if (!demod->isActive()) {
                            demod->setActive(true);
                            if (wxGetApp().getDemodMgr().getLastActiveDemodulator() == NULL) {
                                wxGetApp().getDemodMgr().setActiveDemodulator(demod);
                            }
                        }

                        if (!demod->isActive()) {
                            continue;
                        }
                        if (demod->isFollow()) {
                            demod->setFollow(false);
                        }

                        demodQueue->push(demodDataOut);
                        pushedData = true;
                    }

                    if (!pushedData) {
                        demodDataOut->setRefCount(0);
                    }
                }
            }

            busy_demod.unlock();
        }
        data_in->decRefCount();
    }

    while (!buffers.empty()) {
        DemodulatorThreadIQData *demodDataDel = buffers.front();
        buffers.pop_front();
//        std::lock_guard < std::mutex > lock(demodDataDel->m_mutex);
//        delete demodDataDel;
    }
    if (iqVisualQueue.load() && !iqVisualQueue.load()->empty()) {
        DemodulatorThreadIQData *visualDataDummy;
        iqVisualQueue.load()->pop(visualDataDummy);
    }

    delete visualDataOut;

    std::cout << "SDR post-processing thread done." << std::endl;
}

void SDRPostThread::terminate() {
    terminated = true;
    SDRThreadIQData *dummy = new SDRThreadIQData;
    iqDataInQueue.load()->push(dummy);
}
