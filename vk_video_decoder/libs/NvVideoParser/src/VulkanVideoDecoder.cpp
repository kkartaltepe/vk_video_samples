/*
 * SPDX-FileCopyrightText: Copyright (c) 2021 - 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <stdarg.h>
#include "VulkanVideoParserIf.h"
#include "VulkanVideoDecoder.h"
#include "nvVulkanVideoUtils.h"
#include "nvVulkanVideoParser.h"
#include <algorithm>

VulkanVideoDecoder::VulkanVideoDecoder(VkVideoCodecOperationFlagBitsKHR std)
  : m_refCount(0),
    m_standard(std),
    m_264SvcEnabled(false),
    m_outOfBandPictureParameters(false),
    m_initSequenceIsCalled(false),
    m_pClient(),
    m_defaultMinBufferSize(2 * 1024 * 1024),
    m_bufferOffsetAlignment(256),
    m_bufferSizeAlignment(256),
    m_bitstreamData(),
    m_bitstreamDataLen()
{
    m_bNoStartCodes = false;
    m_lMinBytesForBoundaryDetection = 256;
    m_bFilterTimestamps = false;
    if (m_264SvcEnabled)
    {
        m_pVkPictureData = new VkParserPictureData[128];
    }
    else
    {
        m_pVkPictureData = new VkParserPictureData;
    }
    m_iTargetLayer = 0;
    m_eError = NV_NO_ERROR;
}


VulkanVideoDecoder::~VulkanVideoDecoder()
{
    if (m_264SvcEnabled)
    {
        delete [] m_pVkPictureData;
        m_pVkPictureData = NULL;
    }
    else
    {
        delete m_pVkPictureData;
        m_pVkPictureData = NULL;
    }
}


VkResult VulkanVideoDecoder::Initialize(const VkParserInitDecodeParameters *pParserPictureData)
{

    if (pParserPictureData->interfaceVersion != NV_VULKAN_VIDEO_PARSER_API_VERSION) {
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }

    Deinitialize();
    m_pClient = pParserPictureData->pClient;
    m_defaultMinBufferSize  = pParserPictureData->defaultMinBufferSize;
    m_bufferOffsetAlignment = pParserPictureData->bufferOffsetAlignment;
    m_bufferSizeAlignment   = pParserPictureData->bufferSizeAlignment;
    m_outOfBandPictureParameters = pParserPictureData->outOfBandPictureParameters;
    m_lClockRate = (pParserPictureData->referenceClockRate > 0) ? pParserPictureData->referenceClockRate : 10000000; // Use 10Mhz as default clock
    m_lErrorThreshold = pParserPictureData->errorThreshold;
    m_bDiscontinuityReported = false;
    m_lFrameDuration = 0;
    m_llExpectedPTS = 0;
    m_bNoStartCodes = false;
    m_bFilterTimestamps = false;
    m_lCheckPTS = 16;
    m_bEmulBytesPresent = false;
    m_bFirstPTS = true;
    if (pParserPictureData->pExternalSeqInfo) {
        m_ExtSeqInfo = *pParserPictureData->pExternalSeqInfo;
    } else {
        memset(&m_ExtSeqInfo, 0, sizeof(m_ExtSeqInfo));
    }

    m_bitstreamDataLen = m_defaultMinBufferSize; // dynamically increase size if it's not enough
    VkSharedBaseObj<VulkanBitstreamBuffer> bitstreamBuffer;
    m_pClient->GetBitstreamBuffer(m_bitstreamDataLen,
                                  m_bufferOffsetAlignment, m_bufferSizeAlignment,
                                  nullptr, 0, bitstreamBuffer);
    assert(bitstreamBuffer);
    if (!bitstreamBuffer) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    m_bitstreamDataLen = m_bitstreamData.SetBitstreamBuffer(bitstreamBuffer);
    CreatePrivateContext();
    memset(&m_nalu, 0, sizeof(m_nalu));
    memset(&m_PrevSeqInfo, 0, sizeof(m_PrevSeqInfo));
    memset(&m_DispInfo, 0, sizeof(m_DispInfo));
    memset(&m_PTSQueue, 0, sizeof(m_PTSQueue));
    m_bitstreamData.ResetStreamMarkers();
    m_BitBfr = (uint32_t)~0;
    m_MaxFrameBuffers = 0;
    m_bDecoderInitFailed = false;
    m_llParsedBytes = 0;
    m_llNaluStartLocation = 0;
    m_llFrameStartLocation = 0;
    m_lPTSPos = 0;
    InitParser();
    memset(&m_nalu, 0, sizeof(m_nalu)); // reset nalu again (in case parser used init_dbits during initialization)
    
    return VK_SUCCESS;
}


bool VulkanVideoDecoder::Deinitialize()
{
    FreeContext();
    m_bitstreamData.ResetBitstreamBuffer();
    return true;
}


void VulkanVideoDecoder::init_dbits()
{
    m_nalu.get_offset = m_nalu.start_offset + ((m_bNoStartCodes) ? 0 : 3);  // Skip over start_code_prefix
    m_nalu.get_zerocnt = 0;
    m_nalu.get_emulcnt = 0;
    m_nalu.get_bfr = 0;
    // prime bit buffer
    m_nalu.get_bfroffs = 32;
    skip_bits(0);
}


void VulkanVideoDecoder::skip_bits(uint32_t n)
{
    m_nalu.get_bfroffs += n;
    while (m_nalu.get_bfroffs >= 8)
    {
        m_nalu.get_bfr <<= 8;
        if (m_nalu.get_offset < m_nalu.end_offset)
        {
            VkDeviceSize c = m_bitstreamData[m_nalu.get_offset++];
            if (m_bEmulBytesPresent)
            {
                // detect / discard emulation_prevention_three_byte
                if (m_nalu.get_zerocnt == 2)
                {
                    if (c == 3)
                    {
                        m_nalu.get_zerocnt = 0;
                        c = (m_nalu.get_offset < m_nalu.end_offset) ? m_bitstreamData[m_nalu.get_offset] : 0;
                        m_nalu.get_offset++;
                        m_nalu.get_emulcnt++;
                    }
                }
                if (c != 0)
                    m_nalu.get_zerocnt = 0;
                else
                    m_nalu.get_zerocnt += (m_nalu.get_zerocnt < 2);
            }
            m_nalu.get_bfr |= c;
        } else
        {
            m_nalu.get_offset++;
        }
        m_nalu.get_bfroffs -= 8;
    }
}

void VulkanVideoDecoder::rbsp_trailing_bits()
{
    f(1, 1); // rbsp_stop_one_bit
    while (!byte_aligned())
        f(1, 0); // rbsp_alignment_zero_bit
}

bool VulkanVideoDecoder::more_rbsp_data()
{
    // If the NAL unit contains any non-zero bits past the next bit we have more RBSP data.
    // These non-zero bits may either already be in bfr (first check)
    // or may not have been read yet (second check).
    // Note that the assumption that end() == false implies that there are more unread
    // non-zero bits is invalid for CABAC slices (because of cabac_zero_word). This is not
    // a problem because more_rbsp_data is not used in CABAC slices. 
    return (m_nalu.get_bfr << (m_nalu.get_bfroffs+1)) != 0 || !end();
}

uint32_t VulkanVideoDecoder::u(uint32_t n)
{
    uint32_t bits = 0;
    
    if (n > 0)
    {
        if (n + m_nalu.get_bfroffs <= 32)
        {
            bits = next_bits(n);
            skip_bits(n);
        } else
        {
            // n == 26..32
            bits = next_bits(n-25) << 25;
            skip_bits(n-25);
            bits |= next_bits(25);
            skip_bits(25);
        }
    }
    return bits;
}


// 9.1
uint32_t VulkanVideoDecoder::ue()
{
    int leadingZeroBits, b, codeNum;

    leadingZeroBits = -1;
    for (b = 0; (!b) && (leadingZeroBits<32); leadingZeroBits++)
        b = u(1);

    codeNum = 0;
    if (leadingZeroBits < 32)
    {
        codeNum = (1 << leadingZeroBits) - 1 + u(leadingZeroBits);
    } else
    {
        codeNum = 0xffffffff + u(leadingZeroBits);
    }
    return codeNum;
}


// 9.1.1
int32_t VulkanVideoDecoder::se()
{
    uint32_t eg = ue();  // Table 9-3
    int32_t codeNum;
    
    if (eg & 1)
        codeNum = (int32_t)((eg>>1)+1);
    else
        codeNum = -(int32_t)(eg>>1);
    return codeNum;
}


size_t VulkanVideoDecoder::next_start_code(const uint8_t *pdatain, size_t datasize, bool& found_start_code)
{
    uint32_t bfr = m_BitBfr;
    size_t i = 0;
    do
    {
        bfr = (bfr << 8) | pdatain[i++];
        if ((bfr & 0x00ffffff) == 1) {
            break;
        }
    } while (i < datasize);
    m_BitBfr = bfr;
    found_start_code = ((bfr & 0x00ffffff) == 1);
    return i;
}

bool VulkanVideoDecoder::resizeBitstreamBuffer(VkDeviceSize extraBytes)
{
    // increasing min 2MB size per resizeBitstreamBuffer()
    VkDeviceSize newBitstreamDataLen = m_bitstreamDataLen + std::max<VkDeviceSize>(extraBytes, (2 * 1024 * 1024));

    VkDeviceSize retSize = m_bitstreamData.ResizeBitstreamBuffer(newBitstreamDataLen, m_bitstreamDataLen, 0);
    if (retSize < newBitstreamDataLen)
    {
        assert(!"bitstream buffer resize failed");
        nvParserLog("ERROR: bitstream buffer resize failed\n");
        return false;
    }

    m_bitstreamDataLen = (VkDeviceSize)retSize;
    return true;
}

VkDeviceSize VulkanVideoDecoder::swapBitstreamBuffer(VkDeviceSize copyCurrBuffOffset, VkDeviceSize copyCurrBuffSize)
{
    VkSharedBaseObj<VulkanBitstreamBuffer> currentBitstreamBuffer(m_bitstreamData.GetBitstreamBuffer());
    VkSharedBaseObj<VulkanBitstreamBuffer> newBitstreamBuffer;
    VkDeviceSize newBufferSize = currentBitstreamBuffer->GetMaxSize();
    const uint8_t* pCopyData = nullptr;
    if (copyCurrBuffSize) {
        VkDeviceSize maxSize = 0;
        pCopyData = currentBitstreamBuffer->GetReadOnlyDataPtr(copyCurrBuffOffset, maxSize);
    }
    m_pClient->GetBitstreamBuffer(newBufferSize,
                                  m_bufferOffsetAlignment, m_bufferSizeAlignment,
                                  pCopyData, copyCurrBuffSize, newBitstreamBuffer);
    assert(newBitstreamBuffer);
    if (!newBitstreamBuffer) {
        assert(!"Cound't GetBitstreamBuffer()!");
        return false;
    }
    // m_bitstreamDataLen = newBufferSize;
    return m_bitstreamData.SetBitstreamBuffer(newBitstreamBuffer);
}

bool VulkanVideoDecoder::ParseByteStream(const VkParserBitstreamPacket* pck, size_t *pParsedBytes)
{
    VkDeviceSize curr_data_size = pck->nDataLength;
    unsigned int framesinpkt = 0;
    const uint8_t *pdatain = (curr_data_size > 0) ? pck->pByteStream : NULL;

    if (!m_bitstreamData) { // make sure we're initialized
        return false;
    }

    m_eError = NV_NO_ERROR; // Reset the flag to catch errors if any in current frame

    m_nCallbackEventCount = 0;
    // Handle discontinuity
    if (pck->bDiscontinuity)
    {
        if (!m_bNoStartCodes)
        {
            if (m_nalu.start_offset == 0)
                m_llNaluStartLocation = m_llParsedBytes - m_nalu.end_offset;

            // Pad the data after the NAL unit with start_code_prefix
            // make the room for 3 bytes 
            if (((VkDeviceSize)(m_nalu.end_offset + 3) > m_bitstreamDataLen) &&
                    !resizeBitstreamBuffer(m_nalu.end_offset + 3 - m_bitstreamDataLen)) {
                return false;
            }
            m_bitstreamData.SetSliceStartCodeAtOffset(m_nalu.end_offset);

            // Complete the current NAL unit (if not empty)
            nal_unit();
            // Decode the current picture (NOTE: may be truncated)
            end_of_picture();
            framesinpkt++;

            m_bitstreamDataLen = swapBitstreamBuffer(m_nalu.start_offset, m_nalu.end_offset - m_nalu.start_offset);
        }
        // Reset the PTS queue to prevent timestamps from before the discontinuity to be associated with
        // a frame past the discontinuity
        memset(&m_PTSQueue, 0, sizeof(m_PTSQueue));
        m_bDiscontinuityReported = true;
    }
    // Remember the packet PTS and its location in the byte stream
    if (pck->bPTSValid)
    {
        m_PTSQueue[m_lPTSPos].bPTSValid = true;
        m_PTSQueue[m_lPTSPos].llPTS = pck->llPTS;
        m_PTSQueue[m_lPTSPos].llPTSPos = m_llParsedBytes;
        m_PTSQueue[m_lPTSPos].bDiscontinuity = m_bDiscontinuityReported;
        m_bDiscontinuityReported = false;
        m_lPTSPos = (m_lPTSPos + 1) % MAX_QUEUED_PTS;
    }
    // In case the bitstream is not startcode-based, the input always only contains a single frame
    if (m_bNoStartCodes)
    {
        if (curr_data_size > m_bitstreamDataLen - 4)
        {
            if (!resizeBitstreamBuffer(curr_data_size - (m_bitstreamDataLen - 4)))
                return false;
        }
        if (curr_data_size > 0)
        {
            m_nalu.start_offset = 0;
            m_nalu.end_offset = m_nalu.start_offset + curr_data_size;
            VkSharedBaseObj<VulkanBitstreamBuffer> bitstreamBuffer(m_bitstreamData.GetBitstreamBuffer());
            bitstreamBuffer->CopyDataFromBuffer(pdatain, 0, m_nalu.start_offset, curr_data_size);
            m_llNaluStartLocation = m_llParsedBytes;
            m_llParsedBytes += curr_data_size;
            m_bitstreamData.ResetStreamMarkers();
            init_dbits();
            if (ParseNalUnit() == NALU_SLICE)
            {
                m_llFrameStartLocation = m_llNaluStartLocation;
                m_bitstreamData.AddStreamMarker(0);
                m_nalu.start_offset = m_nalu.end_offset;
                // Decode only one frame if EOP is set and ignore remaining frames in current packet
                if ((!pck->bEOP) || (pck->bEOP && (framesinpkt < 1)))
                {
                    end_of_picture();
                    framesinpkt++;

                    m_bitstreamDataLen = swapBitstreamBuffer(m_nalu.start_offset, m_nalu.end_offset - m_nalu.start_offset);
                }
            }
        }
        m_nalu.start_offset = 0;
        m_nalu.end_offset = 0;
        if (pck->bEOS)
        {
            end_of_stream();
        }
        if (pParsedBytes)
        {
            *pParsedBytes = pck->nDataLength;
        }

        return (m_eError == NV_NO_ERROR ? true : false);
    }
    // Parse start codes
    while (curr_data_size > 0) {

        VkDeviceSize buflen = curr_data_size;

        // If bPartialParsing is set, we return immediately once we decoded or displayed a frame
        if ((pck->bPartialParsing) && (m_nCallbackEventCount != 0))
        {
            break;
        }
        if ((m_nalu.start_offset > 0) && ((m_nalu.end_offset - m_nalu.start_offset) < (int64_t)m_lMinBytesForBoundaryDetection))
        {
            buflen = std::min<VkDeviceSize>(buflen, (m_lMinBytesForBoundaryDetection - (m_nalu.end_offset - m_nalu.start_offset)));
        }
        bool found_start_code = false;
        VkDeviceSize start_offset = next_start_code(pdatain, (size_t)buflen, found_start_code);
        VkDeviceSize data_used = found_start_code ? start_offset : buflen;
        if (data_used > 0)
        {
            if (data_used > (m_bitstreamDataLen - m_nalu.end_offset))
            {
                resizeBitstreamBuffer(data_used - (m_bitstreamDataLen - m_nalu.end_offset));
            }
            VkDeviceSize bytes = std::min<VkDeviceSize>(data_used, m_bitstreamDataLen - m_nalu.end_offset);
            if (bytes > 0) {
                VkSharedBaseObj<VulkanBitstreamBuffer> bitstreamBuffer(m_bitstreamData.GetBitstreamBuffer());
                bitstreamBuffer->CopyDataFromBuffer(pdatain, 0, m_nalu.end_offset, bytes);
            }
            m_nalu.end_offset += bytes;
            m_llParsedBytes += bytes;
            pdatain += data_used;
            curr_data_size -= data_used;
            // Check for picture boundaries before we have the entire NAL data
            if ((m_nalu.start_offset > 0) && (m_nalu.end_offset == (m_nalu.start_offset + (int64_t)m_lMinBytesForBoundaryDetection)))
            {
                init_dbits();
                if (IsPictureBoundary(available_bits() >> 3)) {
                    // Decode only one frame if EOP is set and ignore remaining frames in current packet
                    if ((!pck->bEOP) || (pck->bEOP && (framesinpkt < 1)))
                    {
                        end_of_picture();
                        framesinpkt++;
                    }
                    // This swap will copy to the new buffer most of the time.
                    m_bitstreamDataLen = swapBitstreamBuffer(m_nalu.start_offset, m_nalu.end_offset - m_nalu.start_offset);
                    m_nalu.end_offset -= m_nalu.start_offset;
                    m_nalu.start_offset = 0;
                    m_bitstreamData.ResetStreamMarkers();
                    m_llNaluStartLocation = m_llParsedBytes - m_nalu.end_offset;
                }
            }
        }
        // Did we find a startcode ?
        if (found_start_code)
        {
            if (m_nalu.start_offset == 0) {
                m_llNaluStartLocation = m_llParsedBytes - m_nalu.end_offset;
            }
            // Remove the trailing 00.00.01 from the NAL unit
            m_nalu.end_offset = (m_nalu.end_offset >= 3) ? (m_nalu.end_offset - 3) : 0;
            nal_unit();
            if (m_bDecoderInitFailed)
            {
                return false;
            }
            // Add back the start code prefix for the next NAL unit
            m_bitstreamData.SetSliceStartCodeAtOffset(m_nalu.end_offset);
            m_nalu.end_offset += 3;
        }
    }
    if (pParsedBytes)
    {
        assert(curr_data_size < std::numeric_limits<size_t>::max());
        *pParsedBytes = pck->nDataLength - (size_t)curr_data_size;
    }
    if (pck->bEOP || pck->bEOS)
    {
        if (m_nalu.start_offset == 0)
            m_llNaluStartLocation = m_llParsedBytes - m_nalu.end_offset;
        // Remove the trailing 00.00.01 from the NAL unit
        if (!!m_bitstreamData && (m_nalu.end_offset >= 3) &&
            m_bitstreamData.HasSliceStartCodeAtOffset(m_nalu.end_offset - 3))
        {
            m_nalu.end_offset = m_nalu.end_offset - 3;
        }
        // Complete the current NAL unit (if not empty)
        nal_unit();

        // Pad the data after the NAL unit with start_code_prefix
        if (((VkDeviceSize)(m_nalu.end_offset + 3) > m_bitstreamDataLen) &&
                !resizeBitstreamBuffer(m_nalu.end_offset + 3 - m_bitstreamDataLen)) {
            return false;
        }
        m_bitstreamData.SetSliceStartCodeAtOffset(m_nalu.end_offset);
        m_nalu.end_offset += 3;

        // Decode the current picture
        if ((!pck->bEOP) || (pck->bEOP && framesinpkt < 1))
        {
            end_of_picture();

             m_bitstreamDataLen = swapBitstreamBuffer(0, 0);
        }
        m_nalu.end_offset = 0;
        m_nalu.start_offset = 0;
        m_bitstreamData.ResetStreamMarkers();
        m_llNaluStartLocation = m_llParsedBytes;
        if (pck->bEOS)
        {
            // Flush everything, release all picture buffers
            end_of_stream();
        }
    }

    return (m_eError == NV_NO_ERROR ? true : false);
}


void VulkanVideoDecoder::nal_unit()
{
    if (((m_nalu.end_offset - m_nalu.start_offset) > 3) &&
         m_bitstreamData.HasSliceStartCodeAtOffset(m_nalu.start_offset))
    {
        int nal_type;
        init_dbits();
        if (IsPictureBoundary(available_bits() >> 3))
        {
            if (m_nalu.start_offset > 0)
            {
                end_of_picture();

                // This swap will copy to the new buffer most of the time.
                m_bitstreamDataLen = swapBitstreamBuffer(m_nalu.start_offset, m_nalu.end_offset - m_nalu.start_offset);
                m_nalu.end_offset -= m_nalu.start_offset;
                m_nalu.start_offset = 0;
                m_bitstreamData.ResetStreamMarkers();
                m_llNaluStartLocation = m_llParsedBytes - m_nalu.end_offset;
            }
        }
        init_dbits();
        nal_type = ParseNalUnit();
        switch(nal_type)
        {
        case NALU_SLICE:
            if (m_bitstreamData.GetStreamMarkersCount() < MAX_SLICES)
            {
                if (m_bitstreamData.GetStreamMarkersCount() == 0) {
                    m_llFrameStartLocation = m_llNaluStartLocation;
                }
                assert(m_nalu.start_offset < std::numeric_limits<int32_t>::max());
                m_bitstreamData.AddStreamMarker((uint32_t)m_nalu.start_offset);
            }
            break;
        //case NALU_DISCARD:
        default:
            if ((nal_type == NALU_UNKNOWN) && (m_pClient))
            {
                // Called client for handling unsupported NALUs (or user data)
                const uint8_t* bitstreamDataPtr = m_bitstreamData.GetBitstreamPtr();
                int64_t cbData = (m_nalu.end_offset - m_nalu.start_offset - 3);
                assert((uint64_t)cbData < (uint64_t)std::numeric_limits<size_t>::max());
                m_pClient->UnhandledNALU(bitstreamDataPtr + m_nalu.start_offset + 3, (size_t)cbData);
            }
            m_nalu.end_offset = m_nalu.start_offset;
        }
    } else
    {
        // Discard invalid NALU
        m_nalu.end_offset = m_nalu.start_offset;
    }
    m_nalu.start_offset = m_nalu.end_offset;
}


bool VulkanVideoDecoder::IsSequenceChange(VkParserSequenceInfo *pnvsi)
{
    if (m_pClient)
    {
        if (memcmp(pnvsi, &m_PrevSeqInfo, sizeof(VkParserSequenceInfo)))
            return true;
    }
    return false;
}


int VulkanVideoDecoder::init_sequence(VkParserSequenceInfo *pnvsi)
{
    if (m_pClient != NULL)
    {
        // Detect sequence info changes
        if (memcmp(pnvsi, &m_PrevSeqInfo, sizeof(VkParserSequenceInfo)))
        {
            uint32_t lNumerator, lDenominator;
            memcpy(&m_PrevSeqInfo, pnvsi, sizeof(VkParserSequenceInfo));
            m_MaxFrameBuffers = m_pClient->BeginSequence(&m_PrevSeqInfo);
            if (!m_MaxFrameBuffers)
            {
                m_bDecoderInitFailed = true;
                return 0;
            }
            lNumerator = NV_FRAME_RATE_NUM(pnvsi->frameRate);
            lDenominator = NV_FRAME_RATE_DEN(pnvsi->frameRate);
            // Determine frame duration
            if ((m_lClockRate > 0) && (lNumerator > 0) && (lDenominator > 0))
            {
                m_lFrameDuration = (int32_t)((uint64_t)lDenominator * m_lClockRate / (uint32_t)lNumerator);
            }
            else if (m_lFrameDuration <= 0)
            {
                nvParserLog("WARNING: Unknown frame rate\n");
                // Default to 30Hz for timestamp interpolation
                m_lFrameDuration  = m_lClockRate / 30;
            }
        }
    }
    return m_MaxFrameBuffers;
}


void VulkanVideoDecoder::end_of_picture()
{
    if ((m_nalu.end_offset > 3) && (m_bitstreamData.GetStreamMarkersCount() > 0))
    {
        assert(!m_264SvcEnabled);
        // memset(m_pVkPictureData, 0, (m_264SvcEnabled ? 128 : 1) * sizeof(VkParserPictureData));
        m_pVkPictureData[0] = VkParserPictureData();
        m_pVkPictureData->bitstreamDataOffset = 0;
        m_pVkPictureData->firstSliceIndex = 0;
        m_pVkPictureData->bitstreamData = m_bitstreamData.GetBitstreamBuffer();
        assert((uint64_t)m_nalu.start_offset < (uint64_t)std::numeric_limits<size_t>::max());
        m_pVkPictureData->bitstreamDataLen = (size_t)m_nalu.start_offset;
        m_pVkPictureData->numSlices = m_bitstreamData.GetStreamMarkersCount();
        if(BeginPicture(m_pVkPictureData))
        {
            if ((m_pVkPictureData + m_iTargetLayer)->pCurrPic)
            {
                int32_t lDisp = 0;

                // Find an entry in m_DispInfo
                for (int32_t i = 0; i < MAX_DELAY; i++)
                {
                    if (m_DispInfo[i].pPicBuf == (m_pVkPictureData + m_iTargetLayer)->pCurrPic)
                    {
                        lDisp = i;
                        break;
                    }
                    if ((m_DispInfo[i].pPicBuf == NULL)
                     || ((m_DispInfo[lDisp].pPicBuf != NULL) && (m_DispInfo[i].llPTS - m_DispInfo[lDisp].llPTS < 0)))
                        lDisp = i;
                }
                m_DispInfo[lDisp].pPicBuf = (m_pVkPictureData + m_iTargetLayer)->pCurrPic;
                m_DispInfo[lDisp].bSkipped = false;
                m_DispInfo[lDisp].bDiscontinuity = false;
                m_DispInfo[lDisp].lPOC = (m_pVkPictureData + m_iTargetLayer)->picture_order_count;
                if (((m_pVkPictureData + m_iTargetLayer)->field_pic_flag) && (!(m_pVkPictureData + m_iTargetLayer)->second_field))
                    m_DispInfo[lDisp].lNumFields = 1;
                else
                    m_DispInfo[lDisp].lNumFields = 2 + (m_pVkPictureData + m_iTargetLayer)->repeat_first_field;
                if ((!(m_pVkPictureData + m_iTargetLayer)->second_field) // Ignore PTS of second field if we already got a PTS for the 1st field
                 || (!m_DispInfo[lDisp].bPTSValid))
                {
                    // Find a PTS in the list
                    unsigned int ndx = m_lPTSPos;
                    m_DispInfo[lDisp].bPTSValid = false;
                    m_DispInfo[lDisp].llPTS = m_llExpectedPTS; // Will be updated later on
                    for (int k = 0; k < MAX_QUEUED_PTS; k++)
                    {
                        if ((m_PTSQueue[ndx].bPTSValid) && (m_PTSQueue[ndx].llPTSPos - m_llFrameStartLocation <= (m_bNoStartCodes ? 0 : 3)))
                        {
                            m_DispInfo[lDisp].bPTSValid = true;
                            m_DispInfo[lDisp].llPTS = m_PTSQueue[ndx].llPTS;
                            m_DispInfo[lDisp].bDiscontinuity = m_PTSQueue[ndx].bDiscontinuity;
                            m_PTSQueue[ndx].bPTSValid = false;
                        }
                        ndx = (ndx+1) % MAX_QUEUED_PTS;
                    }
                }
                // Client callback
                if (m_pClient != NULL)
                {
                    // Notify client
                    if (!m_pClient->DecodePicture(m_pVkPictureData))
                    {
                        m_DispInfo[lDisp].bSkipped = true;
                        nvParserLog("WARNING: skipped decoding current picture\n");
                    }
                    else
                    {
                        m_nCallbackEventCount++;
                    }
                }
            }
            else
            {
                nvParserLog("WARNING: no valid render target for current picture\n");
            }
            // Call back codec for post-decode event (display the decoded frame)
            EndPicture();
        }
    }
}


void VulkanVideoDecoder::display_picture(VkPicIf *pPicBuf, bool bEvict)
{
    int32_t i, lDisp = -1;

    for (i = 0; i < MAX_DELAY; i++) {
        if (m_DispInfo[i].pPicBuf == pPicBuf)
        {
            lDisp = i;
            break;
        }
    }

    if (lDisp >= 0) {

        int64_t llPTS;
        if (m_DispInfo[lDisp].bPTSValid) {

            llPTS = m_DispInfo[lDisp].llPTS;
            // If filtering timestamps, look for the earliest PTS and swap with the current one so that the output
            // timestamps are always increasing (in case we're incorrectly getting the DTS instead of the PTS)
            if (m_bFilterTimestamps || (m_lCheckPTS && !m_DispInfo[lDisp].bDiscontinuity)) {

                int32_t lEarliest = lDisp;
                for (i = 0; i < MAX_DELAY; i++) {

                    if ((m_DispInfo[i].bPTSValid) && (m_DispInfo[i].pPicBuf) && (m_DispInfo[i].llPTS - m_DispInfo[lEarliest].llPTS < 0)) {
                        lEarliest = i;
                    }
                }
                if (lEarliest != lDisp) {
                    if (m_lCheckPTS) {
                        m_bFilterTimestamps = true;
                    }
                    nvParserLog("WARNING: Input timestamps do not match display order\n");
                    llPTS = m_DispInfo[lEarliest].llPTS;
                    m_DispInfo[lEarliest].llPTS = m_DispInfo[lDisp].llPTS;
                    m_DispInfo[lDisp].llPTS = llPTS;
                }
                if (m_lCheckPTS) {
                    m_lCheckPTS--;
                }
            }

        } else {

            llPTS = m_llExpectedPTS;
            if (m_bFirstPTS) {
                // The frame with the first timestamp has not been displayed yet: try to guess
                // using the difference in POC value if available
                for (i = 0; i < MAX_DELAY; i++) {
                    if ((m_DispInfo[i].pPicBuf) && (m_DispInfo[i].bPTSValid)) {
                        int lPOCDiff = m_DispInfo[i].lPOC - m_DispInfo[lDisp].lPOC;
                        if (lPOCDiff < m_DispInfo[lDisp].lNumFields)
                            lPOCDiff = m_DispInfo[lDisp].lNumFields;
                        llPTS = m_DispInfo[i].llPTS - ((lPOCDiff*m_lFrameDuration) >> 1);
                        break;
                    }
                }
            }
        }

        if (llPTS - m_llExpectedPTS < -(m_lFrameDuration >> 2)) {

#if 0
            nvParserLog("WARNING: timestamps going backwards (new=%d, prev=%d, diff=%d, frame_duration=%d)!\n",
                (int)llPTS, (int)m_llExpectedPTS, (int)(llPTS - m_llExpectedPTS), (int)m_lFrameDuration);
#endif
        }
        if ((m_pClient != NULL) && (!m_DispInfo[lDisp].bSkipped)) {

            m_pClient->DisplayPicture(pPicBuf, llPTS);
            m_nCallbackEventCount++;
        }

        if (bEvict) {
            m_DispInfo[lDisp].pPicBuf = NULL;
        }
        m_llExpectedPTS = llPTS + (((uint32_t)m_lFrameDuration * (uint32_t)m_DispInfo[lDisp].lNumFields) >> 1);
        m_bFirstPTS = false;

    } else {
        nvParserLog("WARNING: Attempting to display a picture that was not decoded (%p)\n", pPicBuf);
    }
}


void VulkanVideoDecoder::end_of_stream()
{
    EndOfStream();
    // Reset common parser state
    memset(&m_nalu, 0, sizeof(m_nalu));
    memset(&m_PrevSeqInfo, 0, sizeof(m_PrevSeqInfo));
    memset(&m_PTSQueue, 0, sizeof(m_PTSQueue));
    m_bitstreamData.ResetStreamMarkers();
    m_BitBfr = (uint32_t)~0;
    m_llParsedBytes = 0;
    m_llNaluStartLocation = 0;
    m_llFrameStartLocation = 0;
    m_lFrameDuration = 0;
    m_llExpectedPTS = 0;
    m_bFirstPTS = true;
    m_lPTSPos = 0;
    for (int i = 0; i < MAX_DELAY; i++)
    {
        m_DispInfo[i].pPicBuf = NULL;
        m_DispInfo[i].bPTSValid = false;
    }
}

#include "nvVulkanh265ScalingList.h"
#include "VulkanH264Decoder.h"
#include "VulkanH265Decoder.h"

static nvParserLogFuncType gParserLogFunc = nullptr;
static int gLogLevel = 1;

void nvParserErrorLog(const char* format, ...)
{
    if (!gParserLogFunc) {
        return;
    }
    va_list argptr;
    va_start(argptr, format);
    gParserLogFunc(format, argptr);
    va_end(argptr);
}

void nvParserLog(const char* format, ...)
{
    if (!gParserLogFunc || !gLogLevel) {
        return;
    }
    va_list argptr;
    va_start(argptr, format);
    gParserLogFunc(format, argptr);
    va_end(argptr);
}

void nvParserVerboseLog(const char* format, ...)
{
    if (!gParserLogFunc || (gLogLevel < 50)) {
        return;
    }
    va_list argptr;
    va_start(argptr, format);
    gParserLogFunc(format, argptr);
    va_end(argptr);
}

NVPARSER_EXPORT
VkResult CreateVulkanVideoDecodeParser(VkVideoCodecOperationFlagBitsKHR videoCodecOperation,
                                       const VkExtensionProperties* pStdExtensionVersion,
                                       nvParserLogFuncType pParserLogFunc, int logLevel,
                                       const VkParserInitDecodeParameters* pParserPictureData,
                                       VkSharedBaseObj<VulkanVideoDecodeParser>& nvVideoDecodeParser)
{
    gParserLogFunc = pParserLogFunc;
    gLogLevel = logLevel;
    switch((uint32_t)videoCodecOperation)
    {
    case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR:
    {
        if ((pStdExtensionVersion == nullptr) ||
                (0 != strcmp(pStdExtensionVersion->extensionName, VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_EXTENSION_NAME)) ||
                (pStdExtensionVersion->specVersion != VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_SPEC_VERSION)) {
            nvParserErrorLog("The requested decoder h.264 Codec STD version is NOT supported\n");
            nvParserErrorLog("The supported decoder h.264 Codec STD version is version %d of %s\n",
                    VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_SPEC_VERSION, VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_EXTENSION_NAME);
            return VK_ERROR_INCOMPATIBLE_DRIVER;
        }
        VkSharedBaseObj<VulkanH264Decoder> nvVideoH264DecodeParser( new VulkanH264Decoder(videoCodecOperation));
        if (!nvVideoH264DecodeParser) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        nvVideoDecodeParser = nvVideoH264DecodeParser;
    }
        break;
    case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR:
    {
        if ((pStdExtensionVersion == nullptr) ||
                (0 != strcmp(pStdExtensionVersion->extensionName, VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_EXTENSION_NAME)) ||
                (pStdExtensionVersion->specVersion != VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_SPEC_VERSION)) {
            nvParserErrorLog("The requested decoder h.265 Codec STD version is NOT supported\n");
             nvParserErrorLog("The supported decoder h.265 Codec STD version is version %d of %s\n",
                     VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_SPEC_VERSION, VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_EXTENSION_NAME);
             return VK_ERROR_INCOMPATIBLE_DRIVER;
        }
        VkSharedBaseObj<VulkanH265Decoder> nvVideoH265DecodeParser(new VulkanH265Decoder(videoCodecOperation));
        if (!nvVideoH265DecodeParser) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        nvVideoDecodeParser = nvVideoH265DecodeParser;
    }
        break;
    default:
        nvParserErrorLog("Unsupported codec type!!!\n");
    }
    VkResult result = nvVideoDecodeParser->Initialize(pParserPictureData);
    if (result != VK_SUCCESS) {
        nvVideoDecodeParser = nullptr;
    }
    return result;
}
