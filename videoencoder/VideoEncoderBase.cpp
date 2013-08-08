/*
 INTEL CONFIDENTIAL
 Copyright 2011 Intel Corporation All Rights Reserved.
 The source code contained or described herein and all documents related to the source code ("Material") are owned by Intel Corporation or its suppliers or licensors. Title to the Material remains with Intel Corporation or its suppliers and licensors. The Material contains trade secrets and proprietary and confidential information of Intel or its suppliers and licensors. The Material is protected by worldwide copyright and trade secret laws and treaty provisions. No part of the Material may be used, copied, reproduced, modified, published, uploaded, posted, transmitted, distributed, or disclosed in any way without Intel’s prior express written permission.

 No license under any patent, copyright, trade secret or other intellectual property right is granted to or conferred upon you by disclosure or delivery of the Materials, either expressly, by implication, inducement, estoppel or otherwise. Any license under such intellectual property rights must be express and approved by Intel in writing.
 */
#include <string.h>
#include "VideoEncoderLog.h"
#include "VideoEncoderBase.h"
#include "IntelMetadataBuffer.h"
#include <va/va_tpi.h>
#include <va/va_android.h>
#include <va/va_drmcommon.h>
#ifdef IMG_GFX
#include <hal/hal_public.h>
#endif

// API declaration
extern "C" {
VAStatus vaLockSurface(VADisplay dpy,
    VASurfaceID surface,
    unsigned int *fourcc,
    unsigned int *luma_stride,
    unsigned int *chroma_u_stride,
    unsigned int *chroma_v_stride,
    unsigned int *luma_offset,
    unsigned int *chroma_u_offset,
    unsigned int *chroma_v_offset,
    unsigned int *buffer_name,
    void **buffer
);

VAStatus vaUnlockSurface(VADisplay dpy,
    VASurfaceID surface
);
}
VideoEncoderBase::VideoEncoderBase()
    :mInitialized(true)
    ,mStarted(false)
    ,mVADisplay(NULL)
    ,mVAContext(VA_INVALID_ID)
    ,mVAConfig(VA_INVALID_ID)
    ,mVAEntrypoint(VAEntrypointEncSlice)
    ,mCodedBufSize(0)
    ,mNewHeader(false)
    ,mRenderMaxSliceSize(false)
    ,mRenderQP (false)
    ,mRenderAIR(false)
    ,mRenderFrameRate(false)
    ,mRenderBitRate(false)
    ,mRenderHrd(false)
    ,mSeqParamBuf(0)
    ,mPicParamBuf(0)
    ,mSliceParamBuf(0)
    ,mRefSurface(VA_INVALID_SURFACE)
    ,mRecSurface(VA_INVALID_SURFACE)
    ,mAutoRefSurfaces(NULL)
    ,mFrameNum(0)
    ,mAutoReference(false)
    ,mAutoReferenceSurfaceNum(4)
    ,mSliceSizeOverflow(false)
    ,mCurOutputTask(NULL)
    ,mOutCodedBuffer(0)
    ,mCodedBufferMapped(false)
    ,mCurSegment(NULL)
    ,mOffsetInSeg(0)
    ,mTotalSize(0)
    ,mTotalSizeCopied(0)
    ,mFrameSkipped(false)
    ,mSupportedSurfaceMemType(0){

    VAStatus vaStatus = VA_STATUS_SUCCESS;
    // here the display can be any value, use following one
    // just for consistence purpose, so don't define it
    unsigned int display = 0x18C34078;
    int majorVersion = -1;
    int minorVersion = -1;

    setDefaultParams();

    LOG_V("vaGetDisplay \n");
    mVADisplay = vaGetDisplay(&display);
    if (mVADisplay == NULL) {
        LOG_E("vaGetDisplay failed.");
    }

    vaStatus = vaInitialize(mVADisplay, &majorVersion, &minorVersion);
    LOG_V("vaInitialize \n");
    if (vaStatus != VA_STATUS_SUCCESS) {
        LOG_E( "Failed vaInitialize, vaStatus = %d\n", vaStatus);
        mInitialized = false;
    }
}

VideoEncoderBase::~VideoEncoderBase() {

    VAStatus vaStatus = VA_STATUS_SUCCESS;

    stop();

    vaStatus = vaTerminate(mVADisplay);
    LOG_V( "vaTerminate\n");
    if (vaStatus != VA_STATUS_SUCCESS) {
        LOG_W( "Failed vaTerminate, vaStatus = %d\n", vaStatus);
    } else {
        mVADisplay = NULL;
    }
}

Encode_Status VideoEncoderBase::start() {

    Encode_Status ret = ENCODE_SUCCESS;
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    if (!mInitialized) {
        LOGE("Encoder Initialize fail can not start");
        return ENCODE_DRIVER_FAIL;
    }

    if (mStarted) {
        LOG_V("Encoder has been started\n");
        return ENCODE_ALREADY_INIT;
    }

    queryAutoReferenceConfig(mComParams.profile);

    VAConfigAttrib vaAttrib[5];
    vaAttrib[0].type = VAConfigAttribRTFormat;
    vaAttrib[1].type = VAConfigAttribRateControl;
    vaAttrib[2].type = VAConfigAttribEncAutoReference;
    vaAttrib[3].type = VAConfigAttribEncPackedHeaders;
    vaAttrib[4].type = VAConfigAttribEncMaxRefFrames;

    vaStatus = vaGetConfigAttributes(mVADisplay, mComParams.profile,
            VAEntrypointEncSlice, &vaAttrib[0], 5);
    CHECK_VA_STATUS_RETURN("vaGetConfigAttributes");

    mEncPackedHeaders = vaAttrib[3].value;
    mEncMaxRefFrames = vaAttrib[4].value;

    vaAttrib[0].value = VA_RT_FORMAT_YUV420;
    vaAttrib[1].value = mComParams.rcMode;
    vaAttrib[2].value = mAutoReference ? 1 : VA_ATTRIB_NOT_SUPPORTED;

    LOG_V( "======VA Configuration======\n");
    LOG_I( "profile = %d\n", mComParams.profile);
    LOG_I( "mVAEntrypoint = %d\n", mVAEntrypoint);
    LOG_I( "vaAttrib[0].type = %d\n", vaAttrib[0].type);
    LOG_I( "vaAttrib[1].type = %d\n", vaAttrib[1].type);
    LOG_I( "vaAttrib[2].type = %d\n", vaAttrib[2].type);
    LOG_I( "vaAttrib[0].value (Format) = %d\n", vaAttrib[0].value);
    LOG_I( "vaAttrib[1].value (RC mode) = %d\n", vaAttrib[1].value);
    LOG_I( "vaAttrib[2].value (AutoReference) = %d\n", vaAttrib[2].value);

    LOG_V( "vaCreateConfig\n");

    vaStatus = vaCreateConfig(
            mVADisplay, mComParams.profile, mVAEntrypoint,
            &vaAttrib[0], 2, &(mVAConfig));
//            &vaAttrib[0], 3, &(mVAConfig));  //uncomment this after psb_video supports
    CHECK_VA_STATUS_RETURN("vaCreateConfig");

    querySupportedSurfaceMemTypes();

    if (mComParams.rcMode == VA_RC_VCM) {
        // Following three features are only enabled in VCM mode
        mRenderMaxSliceSize = true;
        mRenderAIR = true;
        mRenderBitRate = true;
    }

    LOG_V( "======VA Create Surfaces for Rec/Ref frames ======\n");

    VASurfaceID surfaces[2];
    VASurfaceAttrib attrib_list[2];
    VASurfaceAttribExternalBuffers  external_refbuf;
    uint32_t stride_aligned, height_aligned;
    if(mAutoReference == false){
        stride_aligned = ((mComParams.resolution.width + 15) / 16 ) * 16;
        height_aligned = ((mComParams.resolution.height + 15) / 16 ) * 16;
    }else{
        if(mComParams.profile == VAProfileVP8Version0_3)
        {
           stride_aligned = ((mComParams.resolution.width + 64 + 63) / 64 ) * 64;  //for vsp stride
           height_aligned = ((mComParams.resolution.height + 64 + 63) / 64 ) * 64;
        }
        else
        {
           stride_aligned = ((mComParams.resolution.width + 63) / 64 ) * 64;  //on Merr, stride must be 64 aligned.
           height_aligned = ((mComParams.resolution.height + 31) / 32 ) * 32;
        }
    }

#if 0
    if(mComParams.profile == VAProfileVP8Version0_3)
        attribute_tpi.size = stride_aligned * height_aligned + stride_aligned * ((((mComParams.resolution.height + 1) / 2 + 32)+63)/64) *64;// FW need w*h + w*chrom_height
    else
        attribute_tpi.size = stride_aligned * height_aligned * 3 / 2;
#endif

    if(mAutoReference == false){
        vaStatus = vaCreateSurfaces(mVADisplay, VA_RT_FORMAT_YUV420,
                    stride_aligned, height_aligned, surfaces, 2, NULL, 0);
        mRefSurface = surfaces[0];
        mRecSurface = surfaces[1];
    }else {
        mAutoRefSurfaces = new VASurfaceID [mAutoReferenceSurfaceNum];
        if(mComParams.profile == VAProfileVP8Version0_3)
        {
           setupVP8RefExternalBuf(stride_aligned,height_aligned,&external_refbuf,&attrib_list[0]);
           vaStatus = vaCreateSurfaces(mVADisplay, VA_RT_FORMAT_YUV420,stride_aligned, height_aligned, mAutoRefSurfaces, mAutoReferenceSurfaceNum, NULL, 0);
        }
        else
           vaStatus = vaCreateSurfaces(mVADisplay, VA_RT_FORMAT_YUV420,
                       stride_aligned, height_aligned, mAutoRefSurfaces, mAutoReferenceSurfaceNum, NULL, 0);
    }
    CHECK_VA_STATUS_RETURN("vaCreateSurfaces");

    //Prepare all Surfaces to be added into Context
    uint32_t contextSurfaceCnt;
    if(mAutoReference == false )
        contextSurfaceCnt = 2 + mSrcSurfaceMapList.size();
    else
        contextSurfaceCnt = mAutoReferenceSurfaceNum + mSrcSurfaceMapList.size();

    VASurfaceID *contextSurfaces = new VASurfaceID[contextSurfaceCnt];
    int32_t index = -1;
    android::List<SurfaceMap *>::iterator map_node;

    for(map_node = mSrcSurfaceMapList.begin(); map_node !=  mSrcSurfaceMapList.end(); map_node++)
    {
        contextSurfaces[++index] = (*map_node)->surface;
        (*map_node)->added = true;
    }

    if(mAutoReference == false){
        contextSurfaces[++index] = mRefSurface;
        contextSurfaces[++index] = mRecSurface;
    } else {
        for (int i=0; i < mAutoReferenceSurfaceNum; i++)
            contextSurfaces[++index] = mAutoRefSurfaces[i];
    }

    //Initialize and save the VA context ID
    LOG_V( "vaCreateContext\n");
    vaStatus = vaCreateContext(mVADisplay, mVAConfig,
#ifdef IMG_GFX
            mComParams.resolution.width,
            mComParams.resolution.height,
#else
            stride_aligned,
            height_aligned,
#endif
            VA_PROGRESSIVE, contextSurfaces, contextSurfaceCnt,
            &(mVAContext));
    CHECK_VA_STATUS_RETURN("vaCreateContext");

    delete [] contextSurfaces;

    LOG_I("Success to create libva context width %d, height %d\n",
          mComParams.resolution.width, mComParams.resolution.height);

    uint32_t maxSize = 0;
    ret = getMaxOutSize(&maxSize);
    CHECK_ENCODE_STATUS_RETURN("getMaxOutSize");

    // Create CodedBuffer for output
    VABufferID VACodedBuffer;

    for(uint32_t i = 0; i <mComParams.codedBufNum; i++) {
            vaStatus = vaCreateBuffer(mVADisplay, mVAContext,
                    VAEncCodedBufferType,
                    mCodedBufSize,
                    1, NULL,
                    &VACodedBuffer);
            CHECK_VA_STATUS_RETURN("vaCreateBuffer::VAEncCodedBufferType");

            mVACodedBufferList.push_back(VACodedBuffer);
    }

    if (ret == ENCODE_SUCCESS)
        mStarted = true;

    LOG_V( "end\n");
    return ret;
}

Encode_Status VideoEncoderBase::encode(VideoEncRawBuffer *inBuffer, uint32_t timeout) {

    Encode_Status ret = ENCODE_SUCCESS;
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    if (!mStarted) {
        LOG_E("Encoder has not initialized yet\n");
        return ENCODE_NOT_INIT;
    }

    CHECK_NULL_RETURN_IFFAIL(inBuffer);

    //======Prepare all resources encoder needed=====.

    //Prepare encode vaSurface
    VASurfaceID sid = VA_INVALID_SURFACE;
    ret = manageSrcSurface(inBuffer, &sid);
    CHECK_ENCODE_STATUS_RETURN("manageSrcSurface");

    //Prepare CodedBuffer
    mCodedBuffer_Lock.lock();
    if(mVACodedBufferList.empty()){
        if(timeout == FUNC_BLOCK)
            mCodedBuffer_Cond.wait(mCodedBuffer_Lock);
        else if (timeout > 0)
            if(NO_ERROR != mEncodeTask_Cond.waitRelative(mCodedBuffer_Lock, 1000000*timeout)){
                mCodedBuffer_Lock.unlock();
                LOG_E("Time out wait for Coded buffer.\n");
                return ENCODE_DEVICE_BUSY;
            }
        else {//Nonblock
            mCodedBuffer_Lock.unlock();
            LOG_E("Coded buffer is not ready now.\n");
            return ENCODE_DEVICE_BUSY;
        }
    }

    if(mVACodedBufferList.empty()){
        mCodedBuffer_Lock.unlock();
        return ENCODE_DEVICE_BUSY;
    }
    VABufferID coded_buf = (VABufferID) *(mVACodedBufferList.begin());
    mVACodedBufferList.erase(mVACodedBufferList.begin());
    mCodedBuffer_Lock.unlock();

    LOG_V("CodedBuffer ID 0x%08x\n", coded_buf);

    //All resources are ready, start to assemble EncodeTask
    EncodeTask* task = new EncodeTask();

    task->completed = false;
    task->enc_surface = sid;
    task->coded_buffer = coded_buf;
    task->timestamp = inBuffer->timeStamp;
    task->priv = inBuffer->priv;

    //Setup frame info, like flag ( SYNCFRAME), frame number, type etc
    task->type = inBuffer->type;
    task->flag = inBuffer->flag;
    PrepareFrameInfo(task);

    if(mAutoReference == false){
        //Setup ref /rec frames
        //TODO: B frame support, temporary use same logic
        switch (inBuffer->type) {
            case FTYPE_UNKNOWN:
            case FTYPE_IDR:
            case FTYPE_I:
            case FTYPE_P:
            {
                if(!mFrameSkipped) {
                    VASurfaceID tmpSurface = mRecSurface;
                    mRecSurface = mRefSurface;
                    mRefSurface = tmpSurface;
                }

                task->ref_surface = mRefSurface;
                task->rec_surface = mRecSurface;

                break;
            }
            case FTYPE_B:
            default:
                LOG_V("Something wrong, B frame may not be supported in this mode\n");
                ret = ENCODE_NOT_SUPPORTED;
                goto CLEAN_UP;
        }
    }else {
        task->ref_surface = VA_INVALID_SURFACE;
        task->rec_surface = VA_INVALID_SURFACE;
    }
    //======Start Encoding, add task to list======
    LOG_V("Start Encoding vaSurface=0x%08x\n", task->enc_surface);

    vaStatus = vaBeginPicture(mVADisplay, mVAContext, task->enc_surface);
    CHECK_VA_STATUS_GOTO_CLEANUP("vaBeginPicture");

    ret = sendEncodeCommand(task);
    CHECK_ENCODE_STATUS_CLEANUP("sendEncodeCommand");

    vaStatus = vaEndPicture(mVADisplay, mVAContext);
    CHECK_VA_STATUS_GOTO_CLEANUP("vaEndPicture");

    LOG_V("Add Task %p into Encode Task list\n", task);
    mEncodeTask_Lock.lock();
    mEncodeTaskList.push_back(task);
    mEncodeTask_Cond.signal();
    mEncodeTask_Lock.unlock();

    mFrameNum ++;

    LOG_V("encode return Success\n");

    return ENCODE_SUCCESS;

CLEAN_UP:

    delete task;
    mCodedBuffer_Lock.lock();
    mVACodedBufferList.push_back(coded_buf); //push to CodedBuffer pool again since it is not used
    mCodedBuffer_Cond.signal();
    mCodedBuffer_Lock.unlock();

    LOG_V("encode return error=%x\n", ret);

    return ret;
}

/*
  1. Firstly check if one task is outputting data, if yes, continue outputting, if not try to get one from list.
  2. Due to block/non-block/block with timeout 3 modes, if task is not completed, then sync surface, if yes,
    start output data
  3. Use variable curoutputtask to record task which is getOutput() working on to avoid push again when get failure
    on non-block/block with timeout modes.
  4. if complete all output data, curoutputtask should be set NULL
*/
Encode_Status VideoEncoderBase::getOutput(VideoEncOutputBuffer *outBuffer, uint32_t timeout) {

    Encode_Status ret = ENCODE_SUCCESS;
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    bool useLocalBuffer = false;

    CHECK_NULL_RETURN_IFFAIL(outBuffer);

    if (mCurOutputTask == NULL) {
        mEncodeTask_Lock.lock();
        if(mEncodeTaskList.empty()) {
            LOG_V("getOutput CurrentTask is NULL\n");
            if(timeout == FUNC_BLOCK) {
                LOG_V("waiting for task....\n");
                mEncodeTask_Cond.wait(mEncodeTask_Lock);
            } else if (timeout > 0) {
                LOG_V("waiting for task in % ms....\n", timeout);
                if(NO_ERROR != mEncodeTask_Cond.waitRelative(mEncodeTask_Lock, 1000000*timeout)) {
                    mEncodeTask_Lock.unlock();
                    LOG_E("Time out wait for encode task.\n");
                    return ENCODE_DATA_NOT_READY;
                }
            } else {//Nonblock
                mEncodeTask_Lock.unlock();
                return ENCODE_DATA_NOT_READY;
            }
        }

        if(mEncodeTaskList.empty()){
            mEncodeTask_Lock.unlock();
            return ENCODE_DATA_NOT_READY;
        }
        mCurOutputTask =  *(mEncodeTaskList.begin());
        mEncodeTaskList.erase(mEncodeTaskList.begin());
        mEncodeTask_Lock.unlock();
    }

    //sync/query/wait task if not completed
    if (mCurOutputTask->completed == false) {
        uint8_t *buf = NULL;
        VASurfaceStatus vaSurfaceStatus;

        if (timeout == FUNC_BLOCK) {
            //block mode, direct sync surface to output data

            LOG_I ("block mode, vaSyncSurface ID = 0x%08x\n", mCurOutputTask->enc_surface);
            vaStatus = vaSyncSurface(mVADisplay, mCurOutputTask->enc_surface);
            CHECK_VA_STATUS_GOTO_CLEANUP("vaSyncSurface");

            mOutCodedBuffer = mCurOutputTask->coded_buffer;

            // Check frame skip
            // Need map buffer before calling query surface below to get the right skip frame flag for current frame
            // It is a requirement of video driver
            vaStatus = vaMapBuffer (mVADisplay, mOutCodedBuffer, (void **)&buf);
            vaStatus = vaUnmapBuffer(mVADisplay, mOutCodedBuffer);

            vaStatus = vaQuerySurfaceStatus(mVADisplay, mCurOutputTask->enc_surface,  &vaSurfaceStatus);
            CHECK_VA_STATUS_RETURN("vaQuerySurfaceStatus");
            mFrameSkipped = vaSurfaceStatus & VASurfaceSkipped;

            mCurOutputTask->completed = true;

        } else {
            //For both block with timeout and non-block mode, query surface, if ready, output data
            LOG_I ("non-block mode, vaQuerySurfaceStatus ID = 0x%08x\n", mCurOutputTask->enc_surface);

            vaStatus = vaQuerySurfaceStatus(mVADisplay, mCurOutputTask->enc_surface,  &vaSurfaceStatus);
            if (vaSurfaceStatus & VASurfaceReady) {
                mOutCodedBuffer = mCurOutputTask->coded_buffer;
                mFrameSkipped = vaSurfaceStatus & VASurfaceSkipped;
                mCurOutputTask->completed = true;
                //if need to call SyncSurface again ?

            }	else {//not ready yet
                ret = ENCODE_DATA_NOT_READY;
                goto CLEAN_UP;
            }

        }

    }

    //start to output data
    ret = prepareForOutput(outBuffer, &useLocalBuffer);
    CHECK_ENCODE_STATUS_CLEANUP("prepareForOutput");

    //copy all flags to outBuffer
    outBuffer->flag = mCurOutputTask->flag;
    outBuffer->type = mCurOutputTask->type;
    outBuffer->timeStamp = mCurOutputTask->timestamp;
    outBuffer->priv = mCurOutputTask->priv;

    if (outBuffer->format == OUTPUT_EVERYTHING || outBuffer->format == OUTPUT_FRAME_DATA) {
        ret = outputAllData(outBuffer);
        CHECK_ENCODE_STATUS_CLEANUP("outputAllData");
    }else {
        ret = getExtFormatOutput(outBuffer);
        CHECK_ENCODE_STATUS_CLEANUP("getExtFormatOutput");
    }

    LOG_I("out size for this getOutput call = %d\n", outBuffer->dataSize);

    ret = cleanupForOutput();
    CHECK_ENCODE_STATUS_CLEANUP("cleanupForOutput");

    LOG_V("getOutput return Success, Frame skip is %d\n", mFrameSkipped);

    return ENCODE_SUCCESS;

CLEAN_UP:

    if (outBuffer->data && (useLocalBuffer == true)) {
        delete[] outBuffer->data;
        outBuffer->data = NULL;
        useLocalBuffer = false;
    }

    if (mCodedBufferMapped) {
        vaStatus = vaUnmapBuffer(mVADisplay, mOutCodedBuffer);
        mCodedBufferMapped = false;
        mCurSegment = NULL;
    }

    delete mCurOutputTask;
    mCurOutputTask = NULL;
    mCodedBuffer_Lock.lock();
    mVACodedBufferList.push_back(mOutCodedBuffer);
    mCodedBuffer_Cond.signal();
    mCodedBuffer_Lock.unlock();

    LOG_V("getOutput return error=%x\n", ret);
    return ret;
}

void VideoEncoderBase::flush() {

    LOG_V( "Begin\n");

    // reset the properities
    mFrameNum = 0;

    LOG_V( "end\n");
}

Encode_Status VideoEncoderBase::stop() {

    VAStatus vaStatus = VA_STATUS_SUCCESS;
    Encode_Status ret = ENCODE_SUCCESS;

    LOG_V( "Begin\n");

    // It is possible that above pointers have been allocated
    // before we set mStarted to true
    if (!mStarted) {
        LOG_V("Encoder has been stopped\n");
        return ENCODE_SUCCESS;
    }
    if (mAutoRefSurfaces) {
        delete[] mAutoRefSurfaces;
        mAutoRefSurfaces = NULL;
    }

    mCodedBuffer_Lock.lock();
    mVACodedBufferList.clear();
    mCodedBuffer_Lock.unlock();
    mCodedBuffer_Cond.broadcast();

    //Delete all uncompleted tasks
    mEncodeTask_Lock.lock();
    while(! mEncodeTaskList.empty())
    {
        delete *mEncodeTaskList.begin();
        mEncodeTaskList.erase(mEncodeTaskList.begin());
    }
    mEncodeTask_Lock.unlock();
    mEncodeTask_Cond.broadcast();

    //Release Src Surface Buffer Map, destroy surface manually since it is not added into context
    LOG_V( "Rlease Src Surface Map\n");
    while(! mSrcSurfaceMapList.empty())
    {
        if (! (*mSrcSurfaceMapList.begin())->added) {
            LOG_V( "Rlease the Src Surface Buffer not added into vaContext\n");
            vaDestroySurfaces(mVADisplay, &((*mSrcSurfaceMapList.begin())->surface), 1);
            if ((*mSrcSurfaceMapList.begin())->surface_backup != VA_INVALID_SURFACE)
                vaDestroySurfaces(mVADisplay, &((*mSrcSurfaceMapList.begin())->surface_backup), 1);
        }
        delete (*mSrcSurfaceMapList.begin());
        mSrcSurfaceMapList.erase(mSrcSurfaceMapList.begin());
    }

    LOG_V( "vaDestroyContext\n");
    if (mVAContext != VA_INVALID_ID) {
        vaStatus = vaDestroyContext(mVADisplay, mVAContext);
        CHECK_VA_STATUS_GOTO_CLEANUP("vaDestroyContext");
    }

    LOG_V( "vaDestroyConfig\n");
    if (mVAConfig != VA_INVALID_ID) {
        vaStatus = vaDestroyConfig(mVADisplay, mVAConfig);
        CHECK_VA_STATUS_GOTO_CLEANUP("vaDestroyConfig");
    }

CLEAN_UP:

    mStarted = false;
    mSliceSizeOverflow = false;
    mCurOutputTask= NULL;
    mOutCodedBuffer = 0;
    mCodedBufferMapped = false;
    mCurSegment = NULL;
    mOffsetInSeg =0;
    mTotalSize = 0;
    mTotalSizeCopied = 0;
    mFrameSkipped = false;
    mSupportedSurfaceMemType = 0;

    LOG_V( "end\n");
    return ret;
}

Encode_Status VideoEncoderBase::prepareForOutput(
        VideoEncOutputBuffer *outBuffer, bool *useLocalBuffer) {

    VAStatus vaStatus = VA_STATUS_SUCCESS;
    VACodedBufferSegment *vaCodedSeg = NULL;
    uint32_t status = 0;
    uint8_t *buf = NULL;

    LOG_V( "begin\n");
    // Won't check parameters here as the caller already checked them
    // mCurSegment is NULL means it is first time to be here after finishing encoding a frame
    if (mCurSegment == NULL && !mCodedBufferMapped) {
        LOG_I ("Coded Buffer ID been mapped = 0x%08x\n", mOutCodedBuffer);
        vaStatus = vaMapBuffer (mVADisplay, mOutCodedBuffer, (void **)&buf);
        CHECK_VA_STATUS_RETURN("vaMapBuffer");
        CHECK_NULL_RETURN_IFFAIL(buf);

        mCodedBufferMapped = true;
        mTotalSize = 0;
        mOffsetInSeg = 0;
        mTotalSizeCopied = 0;
        vaCodedSeg = (VACodedBufferSegment *)buf;
        mCurSegment = (VACodedBufferSegment *)buf;

        while (1) {

            mTotalSize += vaCodedSeg->size;
            status = vaCodedSeg->status;
#ifndef IMG_GFX
            uint8_t *pTemp;
            uint32_t ii;
            pTemp = (uint8_t*)vaCodedSeg->buf;
            for(ii = 0; ii < 16;){
                if (*(pTemp + ii) == 0xFF)
                    ii++;
                else
                    break;
            }
            if (ii > 0) {
                mOffsetInSeg = ii;
            }
#endif
            if (!mSliceSizeOverflow) {
                mSliceSizeOverflow = status & VA_CODED_BUF_STATUS_SLICE_OVERFLOW_MASK;
            }

            if (vaCodedSeg->next == NULL)
                break;

            vaCodedSeg = (VACodedBufferSegment *)vaCodedSeg->next;
        }
    }

    // We will support two buffer allocation mode,
    // one is application allocates the buffer and passes to encode,
    // the other is encode allocate memory

    //means  app doesn't allocate the buffer, so _encode will allocate it.
    if (outBuffer->data == NULL) {
        *useLocalBuffer = true;
        outBuffer->data = new  uint8_t[mTotalSize - mTotalSizeCopied + 100];
        if (outBuffer->data == NULL) {
            LOG_E( "outBuffer->data == NULL\n");
            return ENCODE_NO_MEMORY;
        }
        outBuffer->bufferSize = mTotalSize + 100;
        outBuffer->dataSize = 0;
    }

    // Clear all flag for every call
    outBuffer->flag = 0;
    if (mSliceSizeOverflow) outBuffer->flag |= ENCODE_BUFFERFLAG_SLICEOVERFOLOW;

    if (!mCurSegment)
        return ENCODE_FAIL;

    if (mCurSegment->size < mOffsetInSeg) {
        LOG_E("mCurSegment->size < mOffsetInSeg\n");
        return ENCODE_FAIL;
    }

    // Make sure we have data in current segment
    if (mCurSegment->size == mOffsetInSeg) {
        if (mCurSegment->next != NULL) {
            mCurSegment = (VACodedBufferSegment *)mCurSegment->next;
            mOffsetInSeg = 0;
        } else {
            LOG_V("No more data available\n");
            outBuffer->flag |= ENCODE_BUFFERFLAG_DATAINVALID;
            outBuffer->dataSize = 0;
            mCurSegment = NULL;
            return ENCODE_NO_REQUEST_DATA;
        }
    }

    LOG_V( "end\n");
    return ENCODE_SUCCESS;
}

Encode_Status VideoEncoderBase::cleanupForOutput() {

    VAStatus vaStatus = VA_STATUS_SUCCESS;

    //mCurSegment is NULL means all data has been copied out
    if (mCurSegment == NULL && mCodedBufferMapped) {
        vaStatus = vaUnmapBuffer(mVADisplay, mOutCodedBuffer);
        CHECK_VA_STATUS_RETURN("vaUnmapBuffer");
        mCodedBufferMapped = false;
        mTotalSize = 0;
        mOffsetInSeg = 0;
        mTotalSizeCopied = 0;

        delete mCurOutputTask;
        mCurOutputTask = NULL;
        mCodedBuffer_Lock.lock();
        mVACodedBufferList.push_back(mOutCodedBuffer);
        mCodedBuffer_Cond.signal();
        mCodedBuffer_Lock.unlock();

        LOG_V("All data has been outputted, return CodedBuffer 0x%08x to pool\n", mOutCodedBuffer);
    }
    return ENCODE_SUCCESS;
}

Encode_Status VideoEncoderBase::queryProfileLevelConfig(VADisplay dpy, VAProfile profile) {

    VAStatus vaStatus = VA_STATUS_SUCCESS;
    VAEntrypoint entryPtr[8];
    int i, entryPtrNum;

    if(profile ==  VAProfileH264Main) //need to be fixed
        return ENCODE_NOT_SUPPORTED;

    vaStatus = vaQueryConfigEntrypoints(dpy, profile, entryPtr, &entryPtrNum);
    CHECK_VA_STATUS_RETURN("vaQueryConfigEntrypoints");

    for(i=0; i<entryPtrNum; i++){
        if(entryPtr[i] == VAEntrypointEncSlice)
            return ENCODE_SUCCESS;
    }

    return ENCODE_NOT_SUPPORTED;
}

Encode_Status VideoEncoderBase::queryAutoReferenceConfig(VAProfile profile) {

    VAStatus vaStatus = VA_STATUS_SUCCESS;
    VAConfigAttrib attrib_list;
    attrib_list.type = VAConfigAttribEncAutoReference;
    attrib_list.value = VA_ATTRIB_NOT_SUPPORTED;

    vaStatus = vaGetConfigAttributes(mVADisplay, profile, VAEntrypointEncSlice, &attrib_list, 1);
    if(attrib_list.value == VA_ATTRIB_NOT_SUPPORTED )
        mAutoReference = false;
    else
        mAutoReference = true;

    return ENCODE_SUCCESS;
}

Encode_Status VideoEncoderBase::querySupportedSurfaceMemTypes() {

    VAStatus vaStatus = VA_STATUS_SUCCESS;

    unsigned int num = 0;

    VASurfaceAttrib* attribs = NULL;

    //get attribs number
    vaStatus = vaQuerySurfaceAttributes(mVADisplay, mVAConfig, attribs, &num);
    CHECK_VA_STATUS_RETURN("vaGetSurfaceAttributes");

    if (num == 0)
        return ENCODE_SUCCESS;

    attribs = new VASurfaceAttrib[num];

    vaStatus = vaQuerySurfaceAttributes(mVADisplay, mVAConfig, attribs, &num);
    CHECK_VA_STATUS_RETURN("vaGetSurfaceAttributes");

    for(int i = 0; i < num; i ++) {
        if (attribs[i].type == VASurfaceAttribMemoryType) {
            mSupportedSurfaceMemType = attribs[i].value.value.i;
            break;
        }
        else
            continue;
    }

    delete attribs;

    return ENCODE_SUCCESS;
}

Encode_Status VideoEncoderBase::outputAllData(VideoEncOutputBuffer *outBuffer) {

    // Data size been copied for every single call
    uint32_t sizeCopiedHere = 0;
    uint32_t sizeToBeCopied = 0;

    CHECK_NULL_RETURN_IFFAIL(outBuffer->data);

    while (1) {

        LOG_I("mCurSegment->size = %d, mOffsetInSeg = %d\n", mCurSegment->size, mOffsetInSeg);
        LOG_I("outBuffer->bufferSize = %d, sizeCopiedHere = %d, mTotalSizeCopied = %d\n",
              outBuffer->bufferSize, sizeCopiedHere, mTotalSizeCopied);

        if (mCurSegment->size < mOffsetInSeg || outBuffer->bufferSize < sizeCopiedHere) {
            LOG_E("mCurSegment->size < mOffsetInSeg  || outBuffer->bufferSize < sizeCopiedHere\n");
            return ENCODE_FAIL;
        }

        if ((mCurSegment->size - mOffsetInSeg) <= outBuffer->bufferSize - sizeCopiedHere) {
            sizeToBeCopied = mCurSegment->size - mOffsetInSeg;
            memcpy(outBuffer->data + sizeCopiedHere,
                   (uint8_t *)mCurSegment->buf + mOffsetInSeg, sizeToBeCopied);
            sizeCopiedHere += sizeToBeCopied;
            mTotalSizeCopied += sizeToBeCopied;
            mOffsetInSeg = 0;
        } else {
            sizeToBeCopied = outBuffer->bufferSize - sizeCopiedHere;
            memcpy(outBuffer->data + sizeCopiedHere,
                   (uint8_t *)mCurSegment->buf + mOffsetInSeg, outBuffer->bufferSize - sizeCopiedHere);
            mTotalSizeCopied += sizeToBeCopied;
            mOffsetInSeg += sizeToBeCopied;
            outBuffer->dataSize = outBuffer->bufferSize;
            outBuffer->remainingSize = mTotalSize - mTotalSizeCopied;
            outBuffer->flag |= ENCODE_BUFFERFLAG_PARTIALFRAME;
            return ENCODE_BUFFER_TOO_SMALL;
        }

        if (mCurSegment->next == NULL) {
            outBuffer->dataSize = sizeCopiedHere;
            outBuffer->remainingSize = 0;
            outBuffer->flag |= ENCODE_BUFFERFLAG_ENDOFFRAME;
            mCurSegment = NULL;
            return ENCODE_SUCCESS;
        }

        mCurSegment = (VACodedBufferSegment *)mCurSegment->next;
        mOffsetInSeg = 0;
    }
}

void VideoEncoderBase::setDefaultParams() {

    // Set default value for input parameters
    mComParams.profile = VAProfileH264Baseline;
    mComParams.level = 41;
    mComParams.rawFormat = RAW_FORMAT_NV12;
    mComParams.frameRate.frameRateNum = 30;
    mComParams.frameRate.frameRateDenom = 1;
    mComParams.resolution.width = 0;
    mComParams.resolution.height = 0;
    mComParams.intraPeriod = 30;
    mComParams.rcMode = RATE_CONTROL_NONE;
    mComParams.rcParams.initQP = 15;
    mComParams.rcParams.minQP = 0;
    mComParams.rcParams.bitRate = 640000;
    mComParams.rcParams.targetPercentage= 0;
    mComParams.rcParams.windowSize = 0;
    mComParams.rcParams.disableFrameSkip = 0;
    mComParams.rcParams.disableBitsStuffing = 1;
    mComParams.cyclicFrameInterval = 30;
    mComParams.refreshType = VIDEO_ENC_NONIR;
    mComParams.airParams.airMBs = 0;
    mComParams.airParams.airThreshold = 0;
    mComParams.airParams.airAuto = 1;
    mComParams.disableDeblocking = 2;
    mComParams.syncEncMode = false;
    mComParams.codedBufNum = 2;

    mHrdParam.bufferSize = 0;
    mHrdParam.initBufferFullness = 0;

    mStoreMetaDataInBuffers.isEnabled = false;
}

Encode_Status VideoEncoderBase::setParameters(
        VideoParamConfigSet *videoEncParams) {

    Encode_Status ret = ENCODE_SUCCESS;
    CHECK_NULL_RETURN_IFFAIL(videoEncParams);
    LOG_I("Config type = %d\n", (int)videoEncParams->type);

    if (mStarted) {
        LOG_E("Encoder has been initialized, should use setConfig to change configurations\n");
        return ENCODE_ALREADY_INIT;
    }

    switch (videoEncParams->type) {
        case VideoParamsTypeCommon: {

            VideoParamsCommon *paramsCommon =
                    reinterpret_cast <VideoParamsCommon *> (videoEncParams);
            if (paramsCommon->size != sizeof (VideoParamsCommon)) {
                return ENCODE_INVALID_PARAMS;
            }
            if(paramsCommon->codedBufNum < 2)
                paramsCommon->codedBufNum =2;
            mComParams = *paramsCommon;
            break;
        }

        case VideoParamsTypeUpSteamBuffer: {

            VideoParamsUpstreamBuffer *upStreamBuffer =
                    reinterpret_cast <VideoParamsUpstreamBuffer *> (videoEncParams);

            if (upStreamBuffer->size != sizeof (VideoParamsUpstreamBuffer)) {
                return ENCODE_INVALID_PARAMS;
            }

            ret = setUpstreamBuffer(upStreamBuffer);
            break;
        }

        case VideoParamsTypeUsrptrBuffer: {

            // usrptr only can be get
            // this case should not happen
            break;
        }

        case VideoParamsTypeHRD: {
            VideoParamsHRD *hrd =
                    reinterpret_cast <VideoParamsHRD *> (videoEncParams);

            if (hrd->size != sizeof (VideoParamsHRD)) {
                return ENCODE_INVALID_PARAMS;
            }

            mHrdParam.bufferSize = hrd->bufferSize;
            mHrdParam.initBufferFullness = hrd->initBufferFullness;
            mRenderHrd = true;

            break;
        }

        case VideoParamsTypeStoreMetaDataInBuffers: {
            VideoParamsStoreMetaDataInBuffers *metadata =
                    reinterpret_cast <VideoParamsStoreMetaDataInBuffers *> (videoEncParams);

            if (metadata->size != sizeof (VideoParamsStoreMetaDataInBuffers)) {
                return ENCODE_INVALID_PARAMS;
            }

            mStoreMetaDataInBuffers.isEnabled = metadata->isEnabled;

            break;
        }

        case VideoParamsTypeAVC:
        case VideoParamsTypeH263:
        case VideoParamsTypeMP4:
        case VideoParamsTypeVC1:
        case VideoParamsTypeVP8: {
            ret = derivedSetParams(videoEncParams);
            break;
        }

        default: {
            LOG_E ("Wrong ParamType here\n");
            return ENCODE_INVALID_PARAMS;
        }
    }
    return ret;
}

Encode_Status VideoEncoderBase::getParameters(
        VideoParamConfigSet *videoEncParams) {

    Encode_Status ret = ENCODE_SUCCESS;
    CHECK_NULL_RETURN_IFFAIL(videoEncParams);
    LOG_I("Config type = %d\n", (int)videoEncParams->type);

    switch (videoEncParams->type) {
        case VideoParamsTypeCommon: {

            VideoParamsCommon *paramsCommon =
                    reinterpret_cast <VideoParamsCommon *> (videoEncParams);

            if (paramsCommon->size != sizeof (VideoParamsCommon)) {
                return ENCODE_INVALID_PARAMS;
            }
            *paramsCommon = mComParams;
            break;
        }

        case VideoParamsTypeUpSteamBuffer: {

            // Get upstream buffer could happen
            // but not meaningful a lot
            break;
        }

        case VideoParamsTypeUsrptrBuffer: {
            VideoParamsUsrptrBuffer *usrptrBuffer =
                    reinterpret_cast <VideoParamsUsrptrBuffer *> (videoEncParams);

            if (usrptrBuffer->size != sizeof (VideoParamsUsrptrBuffer)) {
                return ENCODE_INVALID_PARAMS;
            }

            ret = getNewUsrptrFromSurface(
                    usrptrBuffer->width, usrptrBuffer->height, usrptrBuffer->format,
                    usrptrBuffer->expectedSize, &(usrptrBuffer->actualSize),
                    &(usrptrBuffer->stride), &(usrptrBuffer->usrPtr));

            break;
        }

        case VideoParamsTypeHRD: {
            VideoParamsHRD *hrd =
                    reinterpret_cast <VideoParamsHRD *> (videoEncParams);

            if (hrd->size != sizeof (VideoParamsHRD)) {
                return ENCODE_INVALID_PARAMS;
            }

            hrd->bufferSize = mHrdParam.bufferSize;
            hrd->initBufferFullness = mHrdParam.initBufferFullness;

            break;
        }

        case VideoParamsTypeStoreMetaDataInBuffers: {
            VideoParamsStoreMetaDataInBuffers *metadata =
                    reinterpret_cast <VideoParamsStoreMetaDataInBuffers *> (videoEncParams);

            if (metadata->size != sizeof (VideoParamsStoreMetaDataInBuffers)) {
                return ENCODE_INVALID_PARAMS;
            }

            metadata->isEnabled = mStoreMetaDataInBuffers.isEnabled;

            break;
        }

        case VideoParamsTypeProfileLevel: {
            VideoParamsProfileLevel *profilelevel =
                reinterpret_cast <VideoParamsProfileLevel *> (videoEncParams);

            if (profilelevel->size != sizeof (VideoParamsProfileLevel)) {
                return ENCODE_INVALID_PARAMS;
            }

            profilelevel->level = 0;
            if(queryProfileLevelConfig(mVADisplay, profilelevel->profile) == ENCODE_SUCCESS){
                profilelevel->isSupported = true;
                if(profilelevel->profile == VAProfileH264High)
                    profilelevel->level = 42;
                else if(profilelevel->profile == VAProfileH264Main)
                     profilelevel->level = 42;
                else if(profilelevel->profile == VAProfileH264Baseline)
                     profilelevel->level = 41;
                else{
                    profilelevel->level = 0;
                    profilelevel->isSupported = false;
                }
            }
        }

        case VideoParamsTypeAVC:
        case VideoParamsTypeH263:
        case VideoParamsTypeMP4:
        case VideoParamsTypeVC1:
        case VideoParamsTypeVP8: {
            derivedGetParams(videoEncParams);
            break;
        }

        default: {
            LOG_E ("Wrong ParamType here\n");
            break;
        }

    }
    return ENCODE_SUCCESS;
}

Encode_Status VideoEncoderBase::setConfig(VideoParamConfigSet *videoEncConfig) {

    Encode_Status ret = ENCODE_SUCCESS;
    CHECK_NULL_RETURN_IFFAIL(videoEncConfig);
    LOG_I("Config type = %d\n", (int)videoEncConfig->type);

   // workaround
#if 0
    if (!mStarted) {
        LOG_E("Encoder has not initialized yet, can't call setConfig\n");
        return ENCODE_NOT_INIT;
    }
#endif

    switch (videoEncConfig->type) {
        case VideoConfigTypeFrameRate: {
            VideoConfigFrameRate *configFrameRate =
                    reinterpret_cast <VideoConfigFrameRate *> (videoEncConfig);

            if (configFrameRate->size != sizeof (VideoConfigFrameRate)) {
                return ENCODE_INVALID_PARAMS;
            }
            mComParams.frameRate = configFrameRate->frameRate;
            mRenderFrameRate = true;
            break;
        }

        case VideoConfigTypeBitRate: {
            VideoConfigBitRate *configBitRate =
                    reinterpret_cast <VideoConfigBitRate *> (videoEncConfig);

            if (configBitRate->size != sizeof (VideoConfigBitRate)) {
                return ENCODE_INVALID_PARAMS;
            }
            mComParams.rcParams = configBitRate->rcParams;
            mRenderBitRate = true;
            break;
        }
        case VideoConfigTypeResolution: {

            // Not Implemented
            break;
        }
        case VideoConfigTypeIntraRefreshType: {

            VideoConfigIntraRefreshType *configIntraRefreshType =
                    reinterpret_cast <VideoConfigIntraRefreshType *> (videoEncConfig);

            if (configIntraRefreshType->size != sizeof (VideoConfigIntraRefreshType)) {
                return ENCODE_INVALID_PARAMS;
            }
            mComParams.refreshType = configIntraRefreshType->refreshType;
            break;
        }

        case VideoConfigTypeCyclicFrameInterval: {
            VideoConfigCyclicFrameInterval *configCyclicFrameInterval =
                    reinterpret_cast <VideoConfigCyclicFrameInterval *> (videoEncConfig);
            if (configCyclicFrameInterval->size != sizeof (VideoConfigCyclicFrameInterval)) {
                return ENCODE_INVALID_PARAMS;
            }

            mComParams.cyclicFrameInterval = configCyclicFrameInterval->cyclicFrameInterval;
            break;
        }

        case VideoConfigTypeAIR: {

            VideoConfigAIR *configAIR = reinterpret_cast <VideoConfigAIR *> (videoEncConfig);

            if (configAIR->size != sizeof (VideoConfigAIR)) {
                return ENCODE_INVALID_PARAMS;
            }

            mComParams.airParams = configAIR->airParams;
            mRenderAIR = true;
            break;
        }
        case VideoConfigTypeAVCIntraPeriod:
        case VideoConfigTypeNALSize:
        case VideoConfigTypeIDRRequest:
        case VideoConfigTypeSliceNum:
        case VideoConfigTypeVP8: {

            ret = derivedSetConfig(videoEncConfig);
            break;
        }
        default: {
            LOG_E ("Wrong Config Type here\n");
            break;
        }
    }
    return ret;
}

Encode_Status VideoEncoderBase::getConfig(VideoParamConfigSet *videoEncConfig) {

    Encode_Status ret = ENCODE_SUCCESS;
    CHECK_NULL_RETURN_IFFAIL(videoEncConfig);
    LOG_I("Config type = %d\n", (int)videoEncConfig->type);

    switch (videoEncConfig->type) {
        case VideoConfigTypeFrameRate: {
            VideoConfigFrameRate *configFrameRate =
                    reinterpret_cast <VideoConfigFrameRate *> (videoEncConfig);

            if (configFrameRate->size != sizeof (VideoConfigFrameRate)) {
                return ENCODE_INVALID_PARAMS;
            }

            configFrameRate->frameRate = mComParams.frameRate;
            break;
        }

        case VideoConfigTypeBitRate: {
            VideoConfigBitRate *configBitRate =
                    reinterpret_cast <VideoConfigBitRate *> (videoEncConfig);

            if (configBitRate->size != sizeof (VideoConfigBitRate)) {
                return ENCODE_INVALID_PARAMS;
            }
            configBitRate->rcParams = mComParams.rcParams;


            break;
        }
        case VideoConfigTypeResolution: {
            // Not Implemented
            break;
        }
        case VideoConfigTypeIntraRefreshType: {

            VideoConfigIntraRefreshType *configIntraRefreshType =
                    reinterpret_cast <VideoConfigIntraRefreshType *> (videoEncConfig);

            if (configIntraRefreshType->size != sizeof (VideoConfigIntraRefreshType)) {
                return ENCODE_INVALID_PARAMS;
            }
            configIntraRefreshType->refreshType = mComParams.refreshType;
            break;
        }

        case VideoConfigTypeCyclicFrameInterval: {
            VideoConfigCyclicFrameInterval *configCyclicFrameInterval =
                    reinterpret_cast <VideoConfigCyclicFrameInterval *> (videoEncConfig);
            if (configCyclicFrameInterval->size != sizeof (VideoConfigCyclicFrameInterval)) {
                return ENCODE_INVALID_PARAMS;
            }

            configCyclicFrameInterval->cyclicFrameInterval = mComParams.cyclicFrameInterval;
            break;
        }

        case VideoConfigTypeAIR: {

            VideoConfigAIR *configAIR = reinterpret_cast <VideoConfigAIR *> (videoEncConfig);

            if (configAIR->size != sizeof (VideoConfigAIR)) {
                return ENCODE_INVALID_PARAMS;
            }

            configAIR->airParams = mComParams.airParams;
            break;
        }
        case VideoConfigTypeAVCIntraPeriod:
        case VideoConfigTypeNALSize:
        case VideoConfigTypeIDRRequest:
        case VideoConfigTypeSliceNum:
        case VideoConfigTypeVP8: {

            ret = derivedGetConfig(videoEncConfig);
            break;
        }
        default: {
            LOG_E ("Wrong ParamType here\n");
            break;
        }
    }
    return ret;
}

void VideoEncoderBase:: PrepareFrameInfo (EncodeTask* task) {
    if (mNewHeader) mFrameNum = 0;
    LOG_I( "mFrameNum = %d   ", mFrameNum);

    updateFrameInfo(task) ;
}

Encode_Status VideoEncoderBase:: updateFrameInfo (EncodeTask* task) {

    task->type = FTYPE_P;

    // determine the picture type
    if (mFrameNum == 0)
        task->type = FTYPE_I;
    if (mComParams.intraPeriod != 0 && ((mFrameNum % mComParams.intraPeriod) == 0))
        task->type = FTYPE_I;

    if (task->type == FTYPE_I)
        task->flag |= ENCODE_BUFFERFLAG_SYNCFRAME;

    return ENCODE_SUCCESS;
}

Encode_Status  VideoEncoderBase::getMaxOutSize (uint32_t *maxSize) {

    uint32_t size = mComParams.resolution.width * mComParams.resolution.height;

    if (maxSize == NULL) {
        LOG_E("maxSize == NULL\n");
        return ENCODE_NULL_PTR;
    }

    LOG_V( "Begin\n");

    if (mCodedBufSize > 0) {
        *maxSize = mCodedBufSize;
        LOG_V ("Already calculate the max encoded size, get the value directly");
        return ENCODE_SUCCESS;
    }

    // base on the rate control mode to calculate the defaule encoded buffer size
    if (mComParams.rcMode == VA_RC_NONE) {
        mCodedBufSize = (size * 400) / (16 * 16);
        // set to value according to QP
    } else {
        mCodedBufSize = mComParams.rcParams.bitRate / 4;
    }

    mCodedBufSize =
        max (mCodedBufSize , (size * 400) / (16 * 16));

    // in case got a very large user input bit rate value
    mCodedBufSize =
        min(mCodedBufSize, (size * 1.5 * 8));
    mCodedBufSize =  (mCodedBufSize + 15) &(~15);

    *maxSize = mCodedBufSize;
    return ENCODE_SUCCESS;
}

Encode_Status VideoEncoderBase::getNewUsrptrFromSurface(
    uint32_t width, uint32_t height, uint32_t format,
    uint32_t expectedSize, uint32_t *outsize, uint32_t *stride, uint8_t **usrptr) {

    Encode_Status ret = ENCODE_FAIL;
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    VASurfaceID surface = VA_INVALID_SURFACE;
    VAImage image;
    uint32_t index = 0;

    SurfaceMap *map = NULL;

    LOG_V( "Begin\n");
    // If encode session has been configured, we can not request surface creation anymore
    if (mStarted) {
        LOG_E( "Already Initialized, can not request VA surface anymore\n");
        return ENCODE_WRONG_STATE;
    }
    if (width<=0 || height<=0 ||outsize == NULL ||stride == NULL || usrptr == NULL) {
        LOG_E("width<=0 || height<=0 || outsize == NULL || stride == NULL ||usrptr == NULL\n");
        return ENCODE_NULL_PTR;
    }

    // Current only NV12 is supported in VA API
    // Through format we can get known the number of planes
    if (format != STRING_TO_FOURCC("NV12")) {
        LOG_W ("Format is not supported\n");
        return ENCODE_NOT_SUPPORTED;
    }

    ValueInfo vinfo;
    vinfo.mode = MEM_MODE_SURFACE;
    vinfo.width = width;
    vinfo.height = height;
    vinfo.lumaStride = width;
    vinfo.size = expectedSize;
    vinfo.format = format;

    surface = CreateSurfaceFromExternalBuf(0, vinfo);
    if (surface == VA_INVALID_SURFACE)
        return ENCODE_DRIVER_FAIL;

    vaStatus = vaDeriveImage(mVADisplay, surface, &image);
    CHECK_VA_STATUS_RETURN("vaDeriveImage");
    LOG_V( "vaDeriveImage Done\n");
    vaStatus = vaMapBuffer(mVADisplay, image.buf, (void **) usrptr);
    CHECK_VA_STATUS_RETURN("vaMapBuffer");

    // make sure the physical page been allocated
    for (index = 0; index < image.data_size; index = index + 4096) {
        unsigned char tmp =  *(*usrptr + index);
        if (tmp == 0)
            *(*usrptr + index) = 0;
    }

    *outsize = image.data_size;
    *stride = image.pitches[0];

    map = new SurfaceMap;
    if (map == NULL) {
        LOG_E( "new SurfaceMap failed\n");
        return ENCODE_NO_MEMORY;
    }

    map->surface = surface;
    map->surface_backup = VA_INVALID_SURFACE;
    map->type = MetadataBufferTypeEncoder;
    map->value = (int32_t)*usrptr;
    map->vinfo.mode = (MemMode)MEM_MODE_USRPTR;
    map->vinfo.handle = 0;
    map->vinfo.size = 0;
    map->vinfo.width = width;
    map->vinfo.height = height;
    map->vinfo.lumaStride = width;
    map->vinfo.chromStride = width;
    map->vinfo.format = VA_FOURCC_NV12;
    map->vinfo.s3dformat = 0xffffffff;
    map->added = false;

    mSrcSurfaceMapList.push_back(map);

    LOG_I( "surface = 0x%08x\n",(uint32_t)surface);
    LOG_I("image->pitches[0] = %d\n", image.pitches[0]);
    LOG_I("image->pitches[1] = %d\n", image.pitches[1]);
    LOG_I("image->offsets[0] = %d\n", image.offsets[0]);
    LOG_I("image->offsets[1] = %d\n", image.offsets[1]);
    LOG_I("image->num_planes = %d\n", image.num_planes);
    LOG_I("image->width = %d\n", image.width);
    LOG_I("image->height = %d\n", image.height);
    LOG_I ("data_size = %d\n", image.data_size);
    LOG_I ("usrptr = 0x%p\n", *usrptr);
    LOG_I ("map->value = 0x%p\n ", (void *)map->value);

    vaStatus = vaUnmapBuffer(mVADisplay, image.buf);
    CHECK_VA_STATUS_RETURN("vaUnmapBuffer");
    vaStatus = vaDestroyImage(mVADisplay, image.image_id);
    CHECK_VA_STATUS_RETURN("vaDestroyImage");

    if (*outsize < expectedSize) {
        LOG_E ("Allocated buffer size is small than the expected size, destroy the surface");
        LOG_I ("Allocated size is %d, expected size is %d\n", *outsize, expectedSize);
        vaStatus = vaDestroySurfaces(mVADisplay, &surface, 1);
        CHECK_VA_STATUS_RETURN("vaDestroySurfaces");
        return ENCODE_FAIL;
    }

    ret = ENCODE_SUCCESS;

    return ret;
}


Encode_Status VideoEncoderBase::setUpstreamBuffer(VideoParamsUpstreamBuffer *upStreamBuffer) {

    Encode_Status status = ENCODE_SUCCESS;

    CHECK_NULL_RETURN_IFFAIL(upStreamBuffer);
    if (upStreamBuffer->bufCnt == 0) {
        LOG_E("bufCnt == 0\n");
        return ENCODE_FAIL;
    }

    for(unsigned int i=0; i < upStreamBuffer->bufCnt; i++) {
        if (findSurfaceMapByValue(upStreamBuffer->bufList[i]) != NULL)  //already mapped
            continue;

        //wrap upstream buffer into vaSurface
        SurfaceMap *map = new SurfaceMap;

        map->surface_backup = VA_INVALID_SURFACE;
        map->type = MetadataBufferTypeUser;
        map->value = upStreamBuffer->bufList[i];
        map->vinfo.mode = (MemMode)upStreamBuffer->bufferMode;
        map->vinfo.handle = (uint32_t)upStreamBuffer->display;
        map->vinfo.size = 0;
        if (upStreamBuffer->bufAttrib) {
            map->vinfo.width = upStreamBuffer->bufAttrib->realWidth;
            map->vinfo.height = upStreamBuffer->bufAttrib->realHeight;
            map->vinfo.lumaStride = upStreamBuffer->bufAttrib->lumaStride;
            map->vinfo.chromStride = upStreamBuffer->bufAttrib->chromStride;
            map->vinfo.format = upStreamBuffer->bufAttrib->format;
        }
        map->vinfo.s3dformat = 0xFFFFFFFF;
        map->added = false;
        status = surfaceMapping(map);

        if (status == ENCODE_SUCCESS)
            mSrcSurfaceMapList.push_back(map);
        else
           delete map;
    }

    return status;
}

Encode_Status VideoEncoderBase::surfaceMappingForSurface(SurfaceMap *map) {

    if (!map)
        return ENCODE_NULL_PTR;

    VAStatus vaStatus = VA_STATUS_SUCCESS;
    Encode_Status ret = ENCODE_SUCCESS;
    VASurfaceID surface;

    //try to get kbufhandle from SurfaceID
    uint32_t fourCC = 0;
    uint32_t lumaStride = 0;
    uint32_t chromaUStride = 0;
    uint32_t chromaVStride = 0;
    uint32_t lumaOffset = 0;
    uint32_t chromaUOffset = 0;
    uint32_t chromaVOffset = 0;
    uint32_t kBufHandle = 0;

    vaStatus = vaLockSurface(
            (VADisplay)map->vinfo.handle, (VASurfaceID)map->value,
            &fourCC, &lumaStride, &chromaUStride, &chromaVStride,
            &lumaOffset, &chromaUOffset, &chromaVOffset, &kBufHandle, NULL);

    CHECK_VA_STATUS_RETURN("vaLockSurface");
    LOG_I("Surface incoming = 0x%08x", map->value);
    LOG_I("lumaStride = %d", lumaStride);
    LOG_I("chromaUStride = %d", chromaUStride);
    LOG_I("chromaVStride = %d", chromaVStride);
    LOG_I("lumaOffset = %d", lumaOffset);
    LOG_I("chromaUOffset = %d", chromaUOffset);
    LOG_I("chromaVOffset = %d", chromaVOffset);
    LOG_I("kBufHandle = 0x%08x", kBufHandle);
    LOG_I("fourCC = %d", fourCC);

    vaStatus = vaUnlockSurface((VADisplay)map->vinfo.handle, (VASurfaceID)map->value);
    CHECK_VA_STATUS_RETURN("vaUnlockSurface");

    ValueInfo vinfo;
    memcpy(&vinfo, &(map->vinfo), sizeof(ValueInfo));
    vinfo.mode = MEM_MODE_KBUFHANDLE;

    surface = CreateSurfaceFromExternalBuf(kBufHandle, vinfo);
    if (surface == VA_INVALID_SURFACE)
        return ENCODE_DRIVER_FAIL;

    LOG_I("Surface ID created from Kbuf = 0x%08x", surface);

    map->surface = surface;

    return ret;
}

Encode_Status VideoEncoderBase::surfaceMappingForGfxHandle(SurfaceMap *map) {

    if (!map)
        return ENCODE_NULL_PTR;

    VAStatus vaStatus = VA_STATUS_SUCCESS;
    Encode_Status ret = ENCODE_SUCCESS;
    VASurfaceID surface;

    LOG_I("surfaceMappingForGfxHandle ......\n");
    LOG_I("lumaStride = %d\n", map->vinfo.lumaStride);
    LOG_I("format = 0x%08x\n", map->vinfo.format);
    LOG_I("width = %d\n", mComParams.resolution.width);
    LOG_I("height = %d\n", mComParams.resolution.height);
    LOG_I("gfxhandle = %d\n", map->value);

    ValueInfo vinfo;
    memcpy(&vinfo, &(map->vinfo), sizeof(ValueInfo));

#ifdef IMG_GFX
    // color fmrat may be OMX_INTEL_COLOR_FormatYUV420PackedSemiPlanar or HAL_PIXEL_FORMAT_NV12
    IMG_native_handle_t* h = (IMG_native_handle_t*) map->value;
    LOG_I("IMG_native_handle_t h->iWidth=%d, h->iHeight=%d, h->iFormat=%x\n", h->iWidth, h->iHeight, h->iFormat);
    vinfo.lumaStride = h->iWidth;
    vinfo.format = h->iFormat;
    vinfo.width = h->iWidth;
    vinfo.height = h->iHeight;
#endif

    surface = CreateSurfaceFromExternalBuf(map->value, vinfo);
    if (surface == VA_INVALID_SURFACE)
        return ENCODE_DRIVER_FAIL;

    map->surface = surface;

    LOG_V("surfaceMappingForGfxHandle: Done");
    return ret;
}

Encode_Status VideoEncoderBase::surfaceMappingForKbufHandle(SurfaceMap *map) {

    if (!map)
        return ENCODE_NULL_PTR;

    LOG_I("surfaceMappingForKbufHandle value=%d\n", map->value);
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    Encode_Status ret = ENCODE_SUCCESS;
    VASurfaceID surface;

    map->vinfo.size = map->vinfo.lumaStride * map->vinfo.height * 1.5;
    surface = CreateSurfaceFromExternalBuf(map->value, map->vinfo);
    if (surface == VA_INVALID_SURFACE)
        return ENCODE_DRIVER_FAIL;
    LOG_I("Surface ID created from Kbuf = 0x%08x", map->value);

    map->surface = surface;

    return ret;
}

#if NO_BUFFER_SHARE
static VAStatus upload_yuv_to_surface(VADisplay va_dpy,
        SurfaceMap *map, VASurfaceID surface_id, int picture_width,
        int picture_height)
{
    VAImage surface_image;
    VAStatus vaStatus;
    unsigned char *surface_p = NULL;
    unsigned char *y_src, *uv_src;
    unsigned char *y_dst, *uv_dst;
    int y_size = map->vinfo.height * map->vinfo.lumaStride;
    int row, col;

    vaStatus = vaDeriveImage(va_dpy, surface_id, &surface_image);
    CHECK_VA_STATUS_RETURN("vaDeriveImage");

    vaStatus = vaMapBuffer(va_dpy, surface_image.buf, (void**)&surface_p);
    CHECK_VA_STATUS_RETURN("vaMapBuffer");

    y_src = (unsigned char*)map->value;
    uv_src = (unsigned char*)map->value + y_size; /* UV offset for NV12 */

    y_dst = surface_p + surface_image.offsets[0];
    uv_dst = surface_p + surface_image.offsets[1]; /* UV offset for NV12 */

    /* Y plane */
    for (row = 0; row < picture_height; row++) {
        memcpy(y_dst, y_src, picture_width);
        y_dst += surface_image.pitches[0];
        y_src += map->vinfo.lumaStride;
    }

    for (row = 0; row < (picture_height / 2); row++) {
        memcpy(uv_dst, uv_src, picture_width);
        uv_dst += surface_image.pitches[1];
        uv_src += map->vinfo.chromStride;
    }

    vaUnmapBuffer(va_dpy, surface_image.buf);
    vaDestroyImage(va_dpy, surface_image.image_id);

    return vaStatus;
}
#endif

Encode_Status VideoEncoderBase::surfaceMappingForMalloc(SurfaceMap *map) {

    if (!map)
        return ENCODE_NULL_PTR;

    VAStatus vaStatus = VA_STATUS_SUCCESS;
    Encode_Status ret = ENCODE_SUCCESS;
    VASurfaceID surface;
#if NO_BUFFER_SHARE
    vaStatus = vaCreateSurfaces(mVADisplay, VA_RT_FORMAT_YUV420,
            map->vinfo.width, map->vinfo.height, &surface, 1,
            NULL, 0);
    CHECK_VA_STATUS_RETURN("vaCreateSurfaces");
    map->surface = surface;

    vaStatus = upload_yuv_to_surface(mVADisplay, map, surface,
            mComParams.resolution.width, mComParams.resolution.height);
    CHECK_ENCODE_STATUS_RETURN("upload_yuv_to_surface");

#else
    surface = CreateSurfaceFromExternalBuf(map->value, map->vinfo);
    if (surface == VA_INVALID_SURFACE)
        return ENCODE_DRIVER_FAIL;

    LOG_I("Surface ID created from Malloc = 0x%08x\n", map->value);

    //Merrifield limitation, should use mAutoReference to check if on Merr
    if ((mComParams.profile == VAProfileVP8Version0_3)||((mAutoReference == false) || (map->vinfo.lumaStride % 64 == 0)))
        map->surface = surface;
    else {
        map->surface_backup = surface;

        //TODO: need optimization for both width/height not aligned case
        VASurfaceID surfaceId;
        unsigned int stride_aligned;
        if(mComParams.profile == VAProfileVP8Version0_3)
            stride_aligned = ((mComParams.resolution.width + 31) / 32 ) * 32;
        else
            stride_aligned = ((mComParams.resolution.width + 63) / 64 ) * 64;

            vaStatus = vaCreateSurfaces(mVADisplay, VA_RT_FORMAT_YUV420,
                    stride_aligned, map->vinfo.height, &surfaceId, 1, NULL, 0);

        map->surface = surfaceId;
        LOG_E("Due to 64 alignment, an alternative Surface ID 0x%08x created\n", surfaceId);
    }
#endif

    return ret;
}

Encode_Status VideoEncoderBase::surfaceMapping(SurfaceMap *map) {

    if (!map)
        return ENCODE_NULL_PTR;

    Encode_Status status;

    LOG_I("surfaceMapping mode=%d, format=%d, lumaStride=%d, width=%d, height=%d, value=%x\n", map->vinfo.mode, map->vinfo.format, map->vinfo.lumaStride, map->vinfo.width, map->vinfo.height, map->value);
    switch (map->vinfo.mode) {
        case MEM_MODE_SURFACE:
            status = surfaceMappingForSurface(map);
            break;
        case MEM_MODE_GFXHANDLE:
            status = surfaceMappingForGfxHandle(map);
            break;
        case MEM_MODE_KBUFHANDLE:
            status = surfaceMappingForKbufHandle(map);
            break;
        case MEM_MODE_MALLOC:
        case MEM_MODE_NONECACHE_USRPTR:
            status = surfaceMappingForMalloc(map);
            break;
        case MEM_MODE_ION:
        case MEM_MODE_V4L2:
        case MEM_MODE_USRPTR:
        case MEM_MODE_CI:
        default:
            status = ENCODE_NOT_SUPPORTED;
            break;
    }

    return status;
}

Encode_Status VideoEncoderBase::manageSrcSurface(VideoEncRawBuffer *inBuffer, VASurfaceID *sid) {

    Encode_Status ret = ENCODE_SUCCESS;
    MetadataBufferType type;
    int32_t value;
    ValueInfo vinfo;
    ValueInfo *pvinfo = &vinfo;
    int32_t *extravalues = NULL;
    unsigned int extravalues_count = 0;

    IntelMetadataBuffer imb;
    SurfaceMap *map = NULL;

    if (mStoreMetaDataInBuffers.isEnabled) {
        //metadatabuffer mode
        LOG_I("in metadata mode, data=%p, size=%d\n", inBuffer->data, inBuffer->size);
        if (imb.UnSerialize(inBuffer->data, inBuffer->size) != IMB_SUCCESS) {
            //fail to parse buffer
            return ENCODE_NO_REQUEST_DATA;
        }

        imb.GetType(type);
        imb.GetValue(value);
    } else {
        //raw mode
        LOG_I("in raw mode, data=%p, size=%d\n", inBuffer->data, inBuffer->size);
        if (! inBuffer->data || inBuffer->size == 0) {
            return ENCODE_NULL_PTR;
        }

        type = MetadataBufferTypeUser;
        value = (int32_t)inBuffer->data;
    }


    //find if mapped
    map = (SurfaceMap*) findSurfaceMapByValue(value);

    if (map) {
        //has mapped, get surfaceID directly
        LOG_I("direct find surface %d from value %x\n", map->surface, value);
        *sid = map->surface;
        if (map->surface_backup != VA_INVALID_SURFACE) {
            //need to copy data
            LOG_I("Need copy surfaces from %x to %x\n", map->surface_backup, *sid);
            ret = copySurfaces(map->surface_backup, *sid);
        }
        return ret;
    }

    //if no found from list, then try to map value with parameters
    LOG_I("not find surface from cache with value %x, start mapping if enough information\n", value);

    if (mStoreMetaDataInBuffers.isEnabled) {

        //if type is MetadataBufferTypeGrallocSource, use default parameters
        if (type == MetadataBufferTypeGrallocSource) {
            vinfo.mode = MEM_MODE_GFXHANDLE;
            vinfo.handle = 0;
            vinfo.size = 0;
            vinfo.width = mComParams.resolution.width;
            vinfo.height = mComParams.resolution.height;
            vinfo.lumaStride = mComParams.resolution.width;
            vinfo.chromStride = mComParams.resolution.width;
            vinfo.format = VA_FOURCC_NV12;
            vinfo.s3dformat = 0xFFFFFFFF;
        } else {
            //get all info mapping needs
            imb.GetValueInfo(pvinfo);
            imb.GetExtraValues(extravalues, extravalues_count);
  	}

    } else {

        //raw mode
        vinfo.mode = MEM_MODE_MALLOC;
        vinfo.handle = 0;
        vinfo.size = inBuffer->size;
        vinfo.width = mComParams.resolution.width;
        vinfo.height = mComParams.resolution.height;
        vinfo.lumaStride = mComParams.resolution.width;
        vinfo.chromStride = mComParams.resolution.width;
        vinfo.format = VA_FOURCC_NV12;
        vinfo.s3dformat = 0xFFFFFFFF;
    }

    /*  Start mapping, if pvinfo is not NULL, then have enough info to map;
     *   if extravalues is not NULL, then need to do more times mapping
     */
    if (pvinfo){
        //map according info, and add to surfacemap list
        map = new SurfaceMap;
        map->surface_backup = VA_INVALID_SURFACE;
        map->type = type;
        map->value = value;
        memcpy(&(map->vinfo), pvinfo, sizeof(ValueInfo));
        map->added = false;

        ret = surfaceMapping(map);
        if (ret == ENCODE_SUCCESS) {
            LOG_I("surface mapping success, map value %x into surface %d\n", value, map->surface);
            mSrcSurfaceMapList.push_back(map);
        } else {
            delete map;
            LOG_E("surface mapping failed, wrong info or meet serious error\n");
            return ret;
        }

        *sid = map->surface;
        if (map->surface_backup != VA_INVALID_SURFACE) {
            //need to copy data
            LOG_I("Need copy surfaces from %x to %x\n", map->surface_backup, *sid);
            ret = copySurfaces(map->surface_backup, *sid);
        }

    } else {
        //can't map due to no info
        LOG_E("surface mapping failed,  missing information\n");
        return ENCODE_NO_REQUEST_DATA;
    }

    if (extravalues) {
        //map more using same ValueInfo
        for(unsigned int i=0; i<extravalues_count; i++) {
            map = new SurfaceMap;
            map->surface_backup = VA_INVALID_SURFACE;
            map->type = type;
            map->value = extravalues[i];
            memcpy(&(map->vinfo), pvinfo, sizeof(ValueInfo));
            map->added = false;

            ret = surfaceMapping(map);
            if (ret == ENCODE_SUCCESS) {
                LOG_I("surface mapping extravalue success, map value %x into surface %d\n", extravalues[i], map->surface);
                mSrcSurfaceMapList.push_back(map);
            } else {
                delete map;
                map = NULL;
                LOG_E( "surface mapping extravalue failed, extravalue is %x\n", extravalues[i]);
            }
        }
    }

    return ret;
}

Encode_Status VideoEncoderBase::renderDynamicBitrate() {
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    LOG_V( "Begin\n\n");
    // disable bits stuffing and skip frame apply to all rate control mode

    VAEncMiscParameterBuffer   *miscEncParamBuf;
    VAEncMiscParameterRateControl *bitrateControlParam;
    VABufferID miscParamBufferID;

    vaStatus = vaCreateBuffer(mVADisplay, mVAContext,
            VAEncMiscParameterBufferType,
            sizeof (VAEncMiscParameterBuffer) + sizeof (VAEncMiscParameterRateControl),
            1, NULL,
            &miscParamBufferID);

    CHECK_VA_STATUS_RETURN("vaCreateBuffer");

    vaStatus = vaMapBuffer(mVADisplay, miscParamBufferID, (void **)&miscEncParamBuf);
    CHECK_VA_STATUS_RETURN("vaMapBuffer");

    miscEncParamBuf->type = VAEncMiscParameterTypeRateControl;
    bitrateControlParam = (VAEncMiscParameterRateControl *)miscEncParamBuf->data;

    bitrateControlParam->bits_per_second = mComParams.rcParams.bitRate;
    bitrateControlParam->initial_qp = mComParams.rcParams.initQP;
    bitrateControlParam->min_qp = mComParams.rcParams.minQP;
    bitrateControlParam->target_percentage = mComParams.rcParams.targetPercentage;
    bitrateControlParam->window_size = mComParams.rcParams.windowSize;
    bitrateControlParam->rc_flags.bits.disable_frame_skip = mComParams.rcParams.disableFrameSkip;
    bitrateControlParam->rc_flags.bits.disable_bit_stuffing = mComParams.rcParams.disableBitsStuffing;
    bitrateControlParam->basic_unit_size = 0;

    LOG_I("bits_per_second = %d\n", bitrateControlParam->bits_per_second);
    LOG_I("initial_qp = %d\n", bitrateControlParam->initial_qp);
    LOG_I("min_qp = %d\n", bitrateControlParam->min_qp);
    LOG_I("target_percentage = %d\n", bitrateControlParam->target_percentage);
    LOG_I("window_size = %d\n", bitrateControlParam->window_size);
    LOG_I("disable_frame_skip = %d\n", bitrateControlParam->rc_flags.bits.disable_frame_skip);
    LOG_I("disable_bit_stuffing = %d\n", bitrateControlParam->rc_flags.bits.disable_bit_stuffing);

    vaStatus = vaUnmapBuffer(mVADisplay, miscParamBufferID);
    CHECK_VA_STATUS_RETURN("vaUnmapBuffer");


    vaStatus = vaRenderPicture(mVADisplay, mVAContext,
            &miscParamBufferID, 1);
    CHECK_VA_STATUS_RETURN("vaRenderPicture");

    return ENCODE_SUCCESS;
}


Encode_Status VideoEncoderBase::renderDynamicFrameRate() {

    VAStatus vaStatus = VA_STATUS_SUCCESS;

    if (mComParams.rcMode != RATE_CONTROL_VCM) {

        LOG_W("Not in VCM mode, but call SendDynamicFramerate\n");
        return ENCODE_SUCCESS;
    }

    VAEncMiscParameterBuffer   *miscEncParamBuf;
    VAEncMiscParameterFrameRate *frameRateParam;
    VABufferID miscParamBufferID;

    vaStatus = vaCreateBuffer(mVADisplay, mVAContext,
            VAEncMiscParameterBufferType,
            sizeof(miscEncParamBuf) + sizeof(VAEncMiscParameterFrameRate),
            1, NULL, &miscParamBufferID);
    CHECK_VA_STATUS_RETURN("vaCreateBuffer");

    vaStatus = vaMapBuffer(mVADisplay, miscParamBufferID, (void **)&miscEncParamBuf);
    CHECK_VA_STATUS_RETURN("vaMapBuffer");

    miscEncParamBuf->type = VAEncMiscParameterTypeFrameRate;
    frameRateParam = (VAEncMiscParameterFrameRate *)miscEncParamBuf->data;
    frameRateParam->framerate =
            (unsigned int) (mComParams.frameRate.frameRateNum + mComParams.frameRate.frameRateDenom/2)
            / mComParams.frameRate.frameRateDenom;

    vaStatus = vaUnmapBuffer(mVADisplay, miscParamBufferID);
    CHECK_VA_STATUS_RETURN("vaUnmapBuffer");

    vaStatus = vaRenderPicture(mVADisplay, mVAContext, &miscParamBufferID, 1);
    CHECK_VA_STATUS_RETURN("vaRenderPicture");

    LOG_I( "frame rate = %d\n", frameRateParam->framerate);
    return ENCODE_SUCCESS;
}

Encode_Status VideoEncoderBase::renderHrd() {

    VAStatus vaStatus = VA_STATUS_SUCCESS;

    VAEncMiscParameterBuffer *miscEncParamBuf;
    VAEncMiscParameterHRD *hrdParam;
    VABufferID miscParamBufferID;

    vaStatus = vaCreateBuffer(mVADisplay, mVAContext,
            VAEncMiscParameterBufferType,
            sizeof(miscEncParamBuf) + sizeof(VAEncMiscParameterHRD),
            1, NULL, &miscParamBufferID);
    CHECK_VA_STATUS_RETURN("vaCreateBuffer");

    vaStatus = vaMapBuffer(mVADisplay, miscParamBufferID, (void **)&miscEncParamBuf);
    CHECK_VA_STATUS_RETURN("vaMapBuffer");

    miscEncParamBuf->type = VAEncMiscParameterTypeHRD;
    hrdParam = (VAEncMiscParameterHRD *)miscEncParamBuf->data;

    hrdParam->buffer_size = mHrdParam.bufferSize;
    hrdParam->initial_buffer_fullness = mHrdParam.initBufferFullness;

    vaStatus = vaUnmapBuffer(mVADisplay, miscParamBufferID);
    CHECK_VA_STATUS_RETURN("vaUnmapBuffer");

    vaStatus = vaRenderPicture(mVADisplay, mVAContext, &miscParamBufferID, 1);
    CHECK_VA_STATUS_RETURN("vaRenderPicture");

    return ENCODE_SUCCESS;
}

SurfaceMap *VideoEncoderBase::findSurfaceMapByValue(int32_t value) {
    android::List<SurfaceMap *>::iterator node;

    for(node = mSrcSurfaceMapList.begin(); node !=  mSrcSurfaceMapList.end(); node++)
    {
        if ((*node)->value == value)
            return *node;
        else
            continue;
    }

    return NULL;
}

Encode_Status VideoEncoderBase::copySurfaces(VASurfaceID srcId, VASurfaceID destId) {

    VAStatus vaStatus = VA_STATUS_SUCCESS;

    uint32_t width = mComParams.resolution.width;
    uint32_t height = mComParams.resolution.height;

    uint32_t i, j;

    VAImage srcImage, destImage;
    uint8_t *pSrcBuffer, *pDestBuffer;

    uint8_t *srcY, *dstY;
    uint8_t *srcU, *srcV;
    uint8_t *srcUV, *dstUV;

    LOG_I("src Surface ID = 0x%08x, dest Surface ID = 0x%08x\n", (uint32_t) srcId, (uint32_t) destId);

    vaStatus = vaDeriveImage(mVADisplay, srcId, &srcImage);
    CHECK_VA_STATUS_RETURN("vaDeriveImage");
    vaStatus = vaMapBuffer(mVADisplay, srcImage.buf, (void **)&pSrcBuffer);
    CHECK_VA_STATUS_RETURN("vaMapBuffer");

    LOG_V("Src Image information\n");
    LOG_I("srcImage.pitches[0] = %d\n", srcImage.pitches[0]);
    LOG_I("srcImage.pitches[1] = %d\n", srcImage.pitches[1]);
    LOG_I("srcImage.offsets[0] = %d\n", srcImage.offsets[0]);
    LOG_I("srcImage.offsets[1] = %d\n", srcImage.offsets[1]);
    LOG_I("srcImage.num_planes = %d\n", srcImage.num_planes);
    LOG_I("srcImage.width = %d\n", srcImage.width);
    LOG_I("srcImage.height = %d\n", srcImage.height);

    vaStatus = vaDeriveImage(mVADisplay, destId, &destImage);
    CHECK_VA_STATUS_RETURN("vaDeriveImage");
    vaStatus = vaMapBuffer(mVADisplay, destImage.buf, (void **)&pDestBuffer);
    CHECK_VA_STATUS_RETURN("vaMapBuffer");

    LOG_V("Dest Image information\n");
    LOG_I("destImage.pitches[0] = %d\n", destImage.pitches[0]);
    LOG_I("destImage.pitches[1] = %d\n", destImage.pitches[1]);
    LOG_I("destImage.offsets[0] = %d\n", destImage.offsets[0]);
    LOG_I("destImage.offsets[1] = %d\n", destImage.offsets[1]);
    LOG_I("destImage.num_planes = %d\n", destImage.num_planes);
    LOG_I("destImage.width = %d\n", destImage.width);
    LOG_I("destImage.height = %d\n", destImage.height);

    if (mComParams.rawFormat == RAW_FORMAT_YUV420) {

        srcY = pSrcBuffer +srcImage.offsets[0];
        srcU = pSrcBuffer + srcImage.offsets[1];
        srcV = pSrcBuffer + srcImage.offsets[2];
        dstY = pDestBuffer + destImage.offsets[0];
        dstUV = pDestBuffer + destImage.offsets[1];

        for (i = 0; i < height; i ++) {
            memcpy(dstY, srcY, width);
            srcY += srcImage.pitches[0];
            dstY += destImage.pitches[0];
        }

        for (i = 0; i < height / 2; i ++) {
            for (j = 0; j < width; j+=2) {
                dstUV [j] = srcU [j / 2];
                dstUV [j + 1] = srcV [j / 2];
            }
            srcU += srcImage.pitches[1];
            srcV += srcImage.pitches[2];
            dstUV += destImage.pitches[1];
        }
    }else if (mComParams.rawFormat == RAW_FORMAT_NV12) {

        srcY = pSrcBuffer + srcImage.offsets[0];
        dstY = pDestBuffer + destImage.offsets[0];
        srcUV = pSrcBuffer + srcImage.offsets[1];
        dstUV = pDestBuffer + destImage.offsets[1];

        for (i = 0; i < height; i++) {
            memcpy(dstY, srcY, width);
            srcY += srcImage.pitches[0];
            dstY += destImage.pitches[0];
        }

        for (i = 0; i < height / 2; i++) {
            memcpy(dstUV, srcUV, width);
            srcUV += srcImage.pitches[1];
            dstUV += destImage.pitches[1];
        }
    } else {
        LOG_E("Raw format not supoort\n");
        return ENCODE_FAIL;
    }

    vaStatus = vaUnmapBuffer(mVADisplay, srcImage.buf);
    CHECK_VA_STATUS_RETURN("vaUnmapBuffer");
    vaStatus = vaDestroyImage(mVADisplay, srcImage.image_id);
    CHECK_VA_STATUS_RETURN("vaDestroyImage");

    vaStatus = vaUnmapBuffer(mVADisplay, destImage.buf);
    CHECK_VA_STATUS_RETURN("vaUnmapBuffer");
    vaStatus = vaDestroyImage(mVADisplay, destImage.image_id);
    CHECK_VA_STATUS_RETURN("vaDestroyImage");

    return ENCODE_SUCCESS;
}

VASurfaceID VideoEncoderBase::CreateSurfaceFromExternalBuf(int32_t value, ValueInfo& vinfo) {
    VAStatus vaStatus;
    VASurfaceAttribExternalBuffers extbuf;
    VASurfaceAttrib attribs[2];
    VASurfaceID surface = VA_INVALID_SURFACE;
    int type;
    unsigned long data = value;

    extbuf.pixel_format = VA_FOURCC_NV12;
    extbuf.width = vinfo.width;
    extbuf.height = vinfo.height;
    extbuf.data_size = vinfo.size;
    if (extbuf.data_size == 0)
        extbuf.data_size = vinfo.lumaStride * vinfo.height * 1.5;
    extbuf.num_buffers = 1;
    extbuf.num_planes = 3;
    extbuf.pitches[0] = vinfo.lumaStride;
    extbuf.pitches[1] = vinfo.lumaStride;
    extbuf.pitches[2] = vinfo.lumaStride;
    extbuf.pitches[3] = 0;
    extbuf.offsets[0] = 0;
    extbuf.offsets[1] = vinfo.lumaStride * vinfo.height;
    extbuf.offsets[2] = extbuf.offsets[1];
    extbuf.offsets[3] = 0;
    extbuf.buffers = &data;
    extbuf.flags = 0;
    extbuf.private_data = NULL;

    switch(vinfo.mode) {
        case MEM_MODE_GFXHANDLE:
            type = VA_SURFACE_ATTRIB_MEM_TYPE_ANDROID_GRALLOC;
            break;
        case MEM_MODE_KBUFHANDLE:
            type = VA_SURFACE_ATTRIB_MEM_TYPE_KERNEL_DRM;
            break;
        case MEM_MODE_MALLOC:
            type = VA_SURFACE_ATTRIB_MEM_TYPE_USER_PTR;
            break;
        case MEM_MODE_NONECACHE_USRPTR:
            type = VA_SURFACE_ATTRIB_MEM_TYPE_USER_PTR;
            extbuf.flags |= VA_SURFACE_EXTBUF_DESC_UNCACHED;
            break;
        case MEM_MODE_SURFACE:
            type = VA_SURFACE_ATTRIB_MEM_TYPE_VA;
            break;
        case MEM_MODE_ION:
        case MEM_MODE_V4L2:
        case MEM_MODE_USRPTR:
        case MEM_MODE_CI:
        default:
            //not support
            return VA_INVALID_SURFACE;
    }

    if (mSupportedSurfaceMemType & type == 0)
        return VA_INVALID_SURFACE;

    attribs[0].type = (VASurfaceAttribType)VASurfaceAttribMemoryType;
    attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[0].value.type = VAGenericValueTypeInteger;
    attribs[0].value.value.i = type;

    attribs[1].type = (VASurfaceAttribType)VASurfaceAttribExternalBufferDescriptor;
    attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[1].value.type = VAGenericValueTypePointer;
    attribs[1].value.value.p = (void *)&extbuf;

    vaStatus = vaCreateSurfaces(mVADisplay, VA_RT_FORMAT_YUV420, vinfo.width,
                                 vinfo.height, &surface, 1, attribs, 2);
    if (vaStatus != VA_STATUS_SUCCESS)
        LOG_E("vaCreateSurfaces failed. vaStatus = %d\n", vaStatus);

    return surface;
}

Encode_Status VideoEncoderBase::setupVP8RefExternalBuf(uint32_t stride_aligned,
                                                       uint32_t height_aligned,
                                                       VASurfaceAttribExternalBuffers *buf,
                                                       VASurfaceAttrib *attrib_list)
{
    int ref_height_uv = (mComParams.resolution.height/ 2 + 32 + 63) & (~63);
    buf->pixel_format = VA_FOURCC_NV12;
    buf->width = stride_aligned;
    buf->height = height_aligned;
    buf->data_size = stride_aligned * height_aligned + stride_aligned * ref_height_uv;
    buf->num_buffers = mAutoReferenceSurfaceNum;
    buf->num_planes = 3;
    buf->pitches[0] = stride_aligned;
    buf->pitches[1] = stride_aligned;
    buf->pitches[2] = stride_aligned;
    buf->pitches[3] = 0;
    buf->offsets[0] = 0;
    buf->offsets[1] = stride_aligned*height_aligned;
    buf->offsets[2] = buf->offsets[1];
    buf->offsets[3] = 0;
    buf->buffers = (unsigned long *)calloc(buf->num_buffers, sizeof(unsigned long));
    buf->flags = 0;
    buf->private_data = NULL;

    attrib_list[0].type = (VASurfaceAttribType)VASurfaceAttribMemoryType;
    attrib_list[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrib_list[0].value.type = VAGenericValueTypeInteger;
    attrib_list[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA;

    attrib_list[1].type = (VASurfaceAttribType)VASurfaceAttribExternalBufferDescriptor;
    attrib_list[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrib_list[1].value.type = VAGenericValueTypePointer;
    attrib_list[1].value.value.p = (void *)buf;

    return 0;
}
