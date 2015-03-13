#include "stdafx.h"
#include "Emu/Memory/Memory.h"
#include "Emu/System.h"
#include "Emu/SysCalls/Modules.h"

#include "stblib/stb_image.h"
#include "Emu/SysCalls/lv2/cellFs.h"
#include "cellPngDec.h"
#include <map>

extern Module cellPngDec;

s32 pngDecCreate(
	vm::ptr<u32> mainHandle,
	vm::ptr<const CellPngDecThreadInParam> param,
	vm::ptr<const CellPngDecExtThreadInParam> ext = vm::ptr<const CellPngDecExtThreadInParam>::make(0))
{
	// alloc memory (should probably use param->cbCtrlMallocFunc)
	auto dec = CellPngDecMainHandle::make(Memory.Alloc(sizeof(PngDecoder), 128));

	if (!dec)
	{
		return CELL_PNGDEC_ERROR_FATAL;
	}

	// initialize decoder
	dec->malloc = param->cbCtrlMallocFunc;
	dec->malloc_arg = param->cbCtrlMallocArg;
	dec->free = param->cbCtrlFreeFunc;
	dec->free_arg = param->cbCtrlFreeArg;

	if (ext)
	{
	}
	
	// use virtual memory address as a handle
	*mainHandle = dec.addr();

	return CELL_OK;
}

s32 pngDecDestroy(CellPngDecMainHandle dec)
{
	if (!Memory.Free(dec.addr()))
	{
		return CELL_PNGDEC_ERROR_FATAL;
	}

	return CELL_OK;
}

s32 pngDecOpen(
	CellPngDecMainHandle dec,
	vm::ptr<u32> subHandle,
	vm::ptr<const CellPngDecSrc> src,
	vm::ptr<CellPngDecOpnInfo> openInfo,
	vm::ptr<const CellPngDecCbCtrlStrm> cb = vm::ptr<const CellPngDecCbCtrlStrm>::make(0),
	vm::ptr<const CellPngDecOpnParam> param = vm::ptr<const CellPngDecOpnParam>::make(0))
{
	// alloc memory (should probably use dec->malloc)
	auto stream = CellPngDecSubHandle::make(Memory.Alloc(sizeof(PngStream), 128));

	if (!stream)
	{
		return CELL_PNGDEC_ERROR_FATAL;
	}
	
	// initialize stream
	stream->fd = 0;
	stream->src = *src;

	switch (src->srcSelect.data())
	{
	case se32(CELL_PNGDEC_BUFFER):
		stream->fileSize = src->streamSize;
		break;

	case se32(CELL_PNGDEC_FILE):
		// Get file descriptor
		vm::var<be_t<u32>> fd;
		int ret = cellFsOpen(src->fileName, 0, fd, vm::ptr<const void>::make(0), 0);
		stream->fd = fd.value();
		if (ret != CELL_OK) return CELL_PNGDEC_ERROR_OPEN_FILE;

		// Get size of file
		vm::var<CellFsStat> sb; // Alloc a CellFsStat struct
		ret = cellFsFstat(stream->fd, sb);
		if (ret != CELL_OK) return ret;
		stream->fileSize = sb->st_size;	// Get CellFsStat.st_size
		break;
	}

	if (cb)
	{
		// TODO: callback
	}

	if (param)
	{
		// TODO: param->selectChunk
	}

	// use virtual memory address as a handle
	*subHandle = stream.addr();

	// set memory info
	openInfo->initSpaceAllocated = 4096;

	return CELL_OK;
}

s32 pngDecClose(CellPngDecSubHandle stream)
{
	cellFsClose(stream->fd);
	if (!Memory.Free(stream.addr()))
	{
		return CELL_PNGDEC_ERROR_FATAL;
	}

	return CELL_OK;
}

s32 pngReadHeader(
	CellPngDecSubHandle stream,
	vm::ptr<CellPngDecInfo> info,
	vm::ptr<CellPngDecExtInfo> extInfo = vm::ptr<CellPngDecExtInfo>::make(0))
{
	CellPngDecInfo& current_info = stream->info;

	if (stream->fileSize < 29)
	{
		return CELL_PNGDEC_ERROR_HEADER; // The file is smaller than the length of a PNG header
	}

	//Write the header to buffer
	vm::var<u8[34]> buffer; // Alloc buffer for PNG header
	auto buffer_32 = buffer.To<be_t<u32>>();
	vm::var<be_t<u64>> pos, nread;

	switch (stream->src.srcSelect.data())
	{
	case se32(CELL_PNGDEC_BUFFER):
		memmove(buffer.begin(), stream->src.streamPtr.get_ptr(), buffer.size());
		break;
	case se32(CELL_PNGDEC_FILE):
		cellFsLseek(stream->fd, 0, CELL_SEEK_SET, pos);
		cellFsRead(stream->fd, vm::ptr<void>::make(buffer.addr()), buffer.size(), nread);
		break;
	}

	if (buffer_32[0].data() != se32(0x89504E47) ||
		buffer_32[1].data() != se32(0x0D0A1A0A) ||  // Error: The first 8 bytes are not a valid PNG signature
		buffer_32[3].data() != se32(0x49484452))   // Error: The PNG file does not start with an IHDR chunk
	{
		return CELL_PNGDEC_ERROR_HEADER;
	}

	switch (buffer[25])
	{
	case 0: current_info.colorSpace = CELL_PNGDEC_GRAYSCALE;       current_info.numComponents = 1; break;
	case 2: current_info.colorSpace = CELL_PNGDEC_RGB;             current_info.numComponents = 3; break;
	case 3: current_info.colorSpace = CELL_PNGDEC_PALETTE;         current_info.numComponents = 1; break;
	case 4: current_info.colorSpace = CELL_PNGDEC_GRAYSCALE_ALPHA; current_info.numComponents = 2; break;
	case 6: current_info.colorSpace = CELL_PNGDEC_RGBA;            current_info.numComponents = 4; break;
	default:
		cellPngDec.Error("cellPngDecDecodeData: Unsupported color space (%d)", (u32)buffer[25]);
		return CELL_PNGDEC_ERROR_HEADER;
	}

	current_info.imageWidth = buffer_32[4];
	current_info.imageHeight = buffer_32[5];
	current_info.bitDepth = buffer[24];
	current_info.interlaceMethod = (CellPngDecInterlaceMode)buffer[28];
	current_info.chunkInformation = 0; // Unimplemented

	*info = current_info;

	if (extInfo)
	{
		extInfo->reserved = 0;
	}

	return CELL_OK;
}

s32 pngDecSetParameter(
	CellPngDecSubHandle stream,
	vm::ptr<const CellPngDecInParam> inParam,
	vm::ptr<CellPngDecOutParam> outParam,
	vm::ptr<const CellPngDecExtInParam> extInParam = vm::ptr<const CellPngDecExtInParam>::make(0),
	vm::ptr<CellPngDecExtOutParam> extOutParam = vm::ptr<CellPngDecExtOutParam>::make(0))
{
	CellPngDecInfo& current_info = stream->info;
	CellPngDecOutParam& current_outParam = stream->outParam;

	current_outParam.outputWidthByte = (current_info.imageWidth * current_info.numComponents * current_info.bitDepth) / 8;
	current_outParam.outputWidth = current_info.imageWidth;
	current_outParam.outputHeight = current_info.imageHeight;
	current_outParam.outputColorSpace = inParam->outputColorSpace;

	switch (current_outParam.outputColorSpace.data())
	{
	case se32(CELL_PNGDEC_PALETTE):
	case se32(CELL_PNGDEC_GRAYSCALE):
		current_outParam.outputComponents = 1; break;

	case se32(CELL_PNGDEC_GRAYSCALE_ALPHA):
		current_outParam.outputComponents = 2; break;

	case se32(CELL_PNGDEC_RGB):
		current_outParam.outputComponents = 3; break;

	case se32(CELL_PNGDEC_RGBA):
	case se32(CELL_PNGDEC_ARGB):
		current_outParam.outputComponents = 4; break;

	default:
		cellPngDec.Error("pngDecSetParameter: Unsupported color space (%d)", current_outParam.outputColorSpace);
		return CELL_PNGDEC_ERROR_ARG;
	}

	current_outParam.outputBitDepth = inParam->outputBitDepth;
	current_outParam.outputMode = inParam->outputMode;
	current_outParam.useMemorySpace = 0; // Unimplemented

	*outParam = current_outParam;

	return CELL_OK;
}

s32 pngDecodeData(
	CellPngDecSubHandle stream,
	vm::ptr<u8> data,
	vm::ptr<const CellPngDecDataCtrlParam> dataCtrlParam,
	vm::ptr<CellPngDecDataOutInfo> dataOutInfo,
	vm::ptr<const CellPngDecCbCtrlDisp> cbCtrlDisp = vm::ptr<const CellPngDecCbCtrlDisp>::make(0),
	vm::ptr<CellPngDecDispParam> dispParam = vm::ptr<CellPngDecDispParam>::make(0))
{
	dataOutInfo->status = CELL_PNGDEC_DEC_STATUS_STOP;

	const u32& fd = stream->fd;
	const u64& fileSize = stream->fileSize;
	const CellPngDecOutParam& current_outParam = stream->outParam;

	//Copy the PNG file to a buffer
	vm::var<unsigned char[]> png((u32)fileSize);
	vm::var<be_t<u64>> pos, nread;

	switch (stream->src.srcSelect.data())
	{
	case se32(CELL_PNGDEC_BUFFER):
		memmove(png.begin(), stream->src.streamPtr.get_ptr(), png.size());
		break;

	case se32(CELL_PNGDEC_FILE):
		cellFsLseek(fd, 0, CELL_SEEK_SET, pos);
		cellFsRead(fd, vm::ptr<void>::make(png.addr()), png.size(), nread);
		break;
	}

	//Decode PNG file. (TODO: Is there any faster alternative? Can we do it without external libraries?)
	int width, height, actual_components;
	auto image = std::unique_ptr<unsigned char, decltype(&::free)>
		(
		stbi_load_from_memory(png.ptr(), (s32)fileSize, &width, &height, &actual_components, 4),
		&::free
		);
	if (!image)
	{
		cellPngDec.Error("pngDecodeData: stbi_load_from_memory failed");
		return CELL_PNGDEC_ERROR_STREAM_FORMAT;
	}

	const bool flip = current_outParam.outputMode == CELL_PNGDEC_BOTTOM_TO_TOP;
	const int bytesPerLine = (u32)dataCtrlParam->outputBytesPerLine;
	uint image_size = width * height;

	switch (current_outParam.outputColorSpace.data())
	{
	case se32(CELL_PNGDEC_RGB):
	case se32(CELL_PNGDEC_RGBA):
	{
		const char nComponents = current_outParam.outputColorSpace == CELL_PNGDEC_RGBA ? 4 : 3;
		image_size *= nComponents;
		if (bytesPerLine > width * nComponents || flip) //check if we need padding
		{
			const int linesize = std::min(bytesPerLine, width * nComponents);
			for (int i = 0; i < height; i++)
			{
				const int dstOffset = i * bytesPerLine;
				const int srcOffset = width * nComponents * (flip ? height - i - 1 : i);
				memcpy(&data[dstOffset], &image.get()[srcOffset], linesize);
			}
		}
		else
		{
			memcpy(data.get_ptr(), image.get(), image_size);
		}
		break;
	}

	case se32(CELL_PNGDEC_ARGB):
	{
		const int nComponents = 4;
		image_size *= nComponents;
		if (bytesPerLine > width * nComponents || flip) //check if we need padding
		{
			//TODO: find out if we can't do padding without an extra copy
			const int linesize = std::min(bytesPerLine, width * nComponents);
			char *output = (char *)malloc(linesize);
			for (int i = 0; i < height; i++)
			{
				const int dstOffset = i * bytesPerLine;
				const int srcOffset = width * nComponents * (flip ? height - i - 1 : i);
				for (int j = 0; j < linesize; j += nComponents)
				{
					output[j + 0] = image.get()[srcOffset + j + 3];
					output[j + 1] = image.get()[srcOffset + j + 0];
					output[j + 2] = image.get()[srcOffset + j + 1];
					output[j + 3] = image.get()[srcOffset + j + 2];
				}
				memcpy(&data[dstOffset], output, linesize);
			}
			free(output);
		}
		else
		{
			uint* img = (uint*)new char[image_size];
			uint* source_current = (uint*)&(image.get()[0]);
			uint* dest_current = img;
			for (uint i = 0; i < image_size / nComponents; i++)
			{
				uint val = *source_current;
				*dest_current = (val >> 24) | (val << 8); // set alpha (A8) as leftmost byte
				source_current++;
				dest_current++;
			}
			memcpy(data.get_ptr(), img, image_size);
			delete[] img;
		}
		break;
	}

	case se32(CELL_PNGDEC_GRAYSCALE):
	case se32(CELL_PNGDEC_PALETTE):
	case se32(CELL_PNGDEC_GRAYSCALE_ALPHA):
		cellPngDec.Error("pngDecodeData: Unsupported color space (%d)", current_outParam.outputColorSpace);
		break;

	default:
		cellPngDec.Error("pngDecodeData: Unsupported color space (%d)", current_outParam.outputColorSpace);
		return CELL_PNGDEC_ERROR_ARG;
	}

	dataOutInfo->status = CELL_PNGDEC_DEC_STATUS_FINISH;

	return CELL_OK;
}

s32 cellPngDecCreate(vm::ptr<u32> mainHandle, vm::ptr<const CellPngDecThreadInParam> threadInParam, vm::ptr<CellPngDecThreadOutParam> threadOutParam)
{
	cellPngDec.Warning("cellPngDecCreate(mainHandle_addr=0x%x, threadInParam_addr=0x%x, threadOutParam_addr=0x%x)",
		mainHandle.addr(), threadInParam.addr(), threadOutParam.addr());

	// create decoder
	if (auto res = pngDecCreate(mainHandle, threadInParam)) return res;

	// set codec version
	threadOutParam->pngCodecVersion = PNGDEC_CODEC_VERSION;

	return CELL_OK;
}

s32 cellPngDecExtCreate(
	vm::ptr<u32> mainHandle,
	vm::ptr<const CellPngDecThreadInParam> threadInParam,
	vm::ptr<CellPngDecThreadOutParam> threadOutParam,
	vm::ptr<const CellPngDecExtThreadInParam> extThreadInParam,
	vm::ptr<CellPngDecExtThreadOutParam> extThreadOutParam)
{
	cellPngDec.Warning("cellPngDecCreate(mainHandle_addr=0x%x, threadInParam_addr=0x%x, threadOutParam_addr=0x%x, extThreadInParam_addr=0x%x, extThreadOutParam_addr=0x%x)",
		mainHandle.addr(), threadInParam.addr(), threadOutParam.addr(), extThreadInParam.addr(), extThreadOutParam.addr());

	// create decoder
	if (auto res = pngDecCreate(mainHandle, threadInParam, extThreadInParam)) return res;

	// set codec version
	threadOutParam->pngCodecVersion = PNGDEC_CODEC_VERSION;

	extThreadOutParam->reserved = 0;

	return CELL_OK;
}

s32 cellPngDecDestroy(CellPngDecMainHandle mainHandle)
{
	cellPngDec.Warning("cellPngDecDestroy(mainHandle=0x%x)", mainHandle.addr());

	// destroy decoder
	return pngDecDestroy(mainHandle);
}

s32 cellPngDecOpen(
	CellPngDecMainHandle mainHandle,
	vm::ptr<u32> subHandle,
	vm::ptr<const CellPngDecSrc> src,
	vm::ptr<CellPngDecOpnInfo> openInfo)
{
	cellPngDec.Warning("cellPngDecOpen(mainHandle=0x%x, subHandle_addr=0x%x, src_addr=0x%x, openInfo_addr=0x%x)",
		mainHandle.addr(), subHandle.addr(), src.addr(), openInfo.addr());

	// create stream handle
	return pngDecOpen(mainHandle, subHandle, src, openInfo);
}

s32 cellPngDecExtOpen(
	CellPngDecMainHandle mainHandle,
	vm::ptr<u32> subHandle,
	vm::ptr<const CellPngDecSrc> src,
	vm::ptr<CellPngDecOpnInfo> openInfo,
	vm::ptr<const CellPngDecCbCtrlStrm> cbCtrlStrm,
	vm::ptr<const CellPngDecOpnParam> opnParam)
{
	cellPngDec.Warning("cellPngDecExtOpen(mainHandle=0x%x, subHandle_addr=0x%x, src_addr=0x%x, openInfo_addr=0x%x, cbCtrlStrm_addr=0x%x, opnParam_addr=0x%x)",
		mainHandle.addr(), subHandle.addr(), src.addr(), openInfo.addr(), cbCtrlStrm.addr(), opnParam.addr());

	// create stream handle
	return pngDecOpen(mainHandle, subHandle, src, openInfo, cbCtrlStrm, opnParam);
}

s32 cellPngDecClose(CellPngDecMainHandle mainHandle, CellPngDecSubHandle subHandle)
{
	cellPngDec.Warning("cellPngDecClose(mainHandle=0x%x, subHandle=0x%x)", mainHandle.addr(), subHandle.addr());

	return pngDecClose(subHandle);
}

s32 cellPngDecReadHeader(CellPngDecMainHandle mainHandle, CellPngDecSubHandle subHandle, vm::ptr<CellPngDecInfo> info)
{
	cellPngDec.Warning("cellPngDecReadHeader(mainHandle=0x%x, subHandle=0x%x, info_addr=0x%x)",
		mainHandle.addr(), subHandle.addr(), info.addr());

	return pngReadHeader(subHandle, info);
}

s32 cellPngDecExtReadHeader(
	CellPngDecMainHandle mainHandle,
	CellPngDecSubHandle subHandle,
	vm::ptr<CellPngDecInfo> info,
	vm::ptr<CellPngDecExtInfo> extInfo)
{
	cellPngDec.Warning("cellPngDecExtReadHeader(mainHandle=0x%x, subHandle=0x%x, info_addr=0x%x, extInfo_addr=0x%x)",
		mainHandle.addr(), subHandle.addr(), info.addr(), extInfo.addr());

	return pngReadHeader(subHandle, info, extInfo);
}

s32 cellPngDecSetParameter(
	CellPngDecMainHandle mainHandle,
	CellPngDecSubHandle subHandle,
	vm::ptr<const CellPngDecInParam> inParam,
	vm::ptr<CellPngDecOutParam> outParam)
{
	cellPngDec.Warning("cellPngDecSetParameter(mainHandle=0x%x, subHandle=0x%x, inParam_addr=0x%x, outParam_addr=0x%x)",
		mainHandle.addr(), subHandle.addr(), inParam.addr(), outParam.addr());

	return pngDecSetParameter(subHandle, inParam, outParam);
}

s32 cellPngDecExtSetParameter(
	CellPngDecMainHandle mainHandle,
	CellPngDecSubHandle subHandle,
	vm::ptr<const CellPngDecInParam> inParam,
	vm::ptr<CellPngDecOutParam> outParam,
	vm::ptr<const CellPngDecExtInParam> extInParam,
	vm::ptr<CellPngDecExtOutParam> extOutParam)
{
	cellPngDec.Warning("cellPngDecExtSetParameter(mainHandle=0x%x, subHandle=0x%x, inParam_addr=0x%x, outParam_addr=0x%x, extInParam=0x%x, extOutParam=0x%x",
		mainHandle.addr(), subHandle.addr(), inParam.addr(), outParam.addr(), extInParam.addr(), extOutParam.addr());

	return pngDecSetParameter(subHandle, inParam, outParam, extInParam, extOutParam);
}

s32 cellPngDecDecodeData(
	CellPngDecMainHandle mainHandle,
	CellPngDecSubHandle subHandle,
	vm::ptr<u8> data,
	vm::ptr<const CellPngDecDataCtrlParam> dataCtrlParam,
	vm::ptr<CellPngDecDataOutInfo> dataOutInfo)
{
	cellPngDec.Warning("cellPngDecDecodeData(mainHandle=0x%x, subHandle=0x%x, data_addr=0x%x, dataCtrlParam_addr=0x%x, dataOutInfo_addr=0x%x)",
		mainHandle.addr(), subHandle.addr(), data.addr(), dataCtrlParam.addr(), dataOutInfo.addr());

	return pngDecodeData(subHandle, data, dataCtrlParam, dataOutInfo);
}

s32 cellPngDecExtDecodeData(
	CellPngDecMainHandle mainHandle,
	CellPngDecSubHandle subHandle,
	vm::ptr<u8> data,
	vm::ptr<const CellPngDecDataCtrlParam> dataCtrlParam,
	vm::ptr<CellPngDecDataOutInfo> dataOutInfo,
	vm::ptr<const CellPngDecCbCtrlDisp> cbCtrlDisp,
	vm::ptr<CellPngDecDispParam> dispParam)
{
	cellPngDec.Warning("cellPngDecExtDecodeData(mainHandle=0x%x, subHandle=0x%x, data_addr=0x%x, dataCtrlParam_addr=0x%x, dataOutInfo_addr=0x%x, cbCtrlDisp_addr=0x%x, dispParam_addr=0x%x)",
		mainHandle.addr(), subHandle.addr(), data.addr(), dataCtrlParam.addr(), dataOutInfo.addr(), cbCtrlDisp.addr(), dispParam.addr());

	return pngDecodeData(subHandle, data, dataCtrlParam, dataOutInfo, cbCtrlDisp, dispParam);
}

s32 cellPngDecGetUnknownChunks(
	CellPngDecMainHandle mainHandle,
	CellPngDecSubHandle subHandle,
	vm::ptr<vm::bptr<CellPngUnknownChunk>> unknownChunk,
	vm::ptr<u32> unknownChunkNumber)
{
	UNIMPLEMENTED_FUNC(cellPngDec);
	return CELL_OK;
}

s32 cellPngDecGetpCAL(CellPngDecMainHandle mainHandle, CellPngDecSubHandle subHandle, vm::ptr<CellPngPCAL> pcal)
{
	UNIMPLEMENTED_FUNC(cellPngDec);
	return CELL_OK;
}

s32 cellPngDecGetcHRM(CellPngDecMainHandle mainHandle, CellPngDecSubHandle subHandle, vm::ptr<CellPngCHRM> chrm)
{
	UNIMPLEMENTED_FUNC(cellPngDec);
	return CELL_OK;
}

s32 cellPngDecGetsCAL(CellPngDecMainHandle mainHandle, CellPngDecSubHandle subHandle, vm::ptr<CellPngSCAL> scal)
{
	UNIMPLEMENTED_FUNC(cellPngDec);
	return CELL_OK;
}

s32 cellPngDecGetpHYs(CellPngDecMainHandle mainHandle, CellPngDecSubHandle subHandle, vm::ptr<CellPngPHYS> phys)
{
	UNIMPLEMENTED_FUNC(cellPngDec);
	return CELL_OK;
}

s32 cellPngDecGetoFFs(CellPngDecMainHandle mainHandle, CellPngDecSubHandle subHandle, vm::ptr<CellPngOFFS> offs)
{
	UNIMPLEMENTED_FUNC(cellPngDec);
	return CELL_OK;
}

s32 cellPngDecGetsPLT(CellPngDecMainHandle mainHandle, CellPngDecSubHandle subHandle, vm::ptr<CellPngSPLT> splt)
{
	UNIMPLEMENTED_FUNC(cellPngDec);
	return CELL_OK;
}

s32 cellPngDecGetbKGD(CellPngDecMainHandle mainHandle, CellPngDecSubHandle subHandle, vm::ptr<CellPngBKGD> bkgd)
{
	UNIMPLEMENTED_FUNC(cellPngDec);
	return CELL_OK;
}

s32 cellPngDecGettIME(CellPngDecMainHandle mainHandle, CellPngDecSubHandle subHandle, vm::ptr<CellPngTIME> time)
{
	UNIMPLEMENTED_FUNC(cellPngDec);
	return CELL_OK;
}

s32 cellPngDecGethIST(CellPngDecMainHandle mainHandle, CellPngDecSubHandle subHandle, vm::ptr<CellPngHIST> hist)
{
	UNIMPLEMENTED_FUNC(cellPngDec);
	return CELL_OK;
}

s32 cellPngDecGettRNS(CellPngDecMainHandle mainHandle, CellPngDecSubHandle subHandle, vm::ptr<CellPngTRNS> trns)
{
	UNIMPLEMENTED_FUNC(cellPngDec);
	return CELL_OK;
}

s32 cellPngDecGetsBIT(CellPngDecMainHandle mainHandle, CellPngDecSubHandle subHandle, vm::ptr<CellPngSBIT> sbit)
{
	UNIMPLEMENTED_FUNC(cellPngDec);
	return CELL_OK;
}

s32 cellPngDecGetiCCP(CellPngDecMainHandle mainHandle, CellPngDecSubHandle subHandle, vm::ptr<CellPngICCP> iccp)
{
	UNIMPLEMENTED_FUNC(cellPngDec);
	return CELL_OK;
}

s32 cellPngDecGetsRGB(CellPngDecMainHandle mainHandle, CellPngDecSubHandle subHandle, vm::ptr<CellPngSRGB> srgb)
{
	UNIMPLEMENTED_FUNC(cellPngDec);
	return CELL_OK;
}

s32 cellPngDecGetgAMA(CellPngDecMainHandle mainHandle, CellPngDecSubHandle subHandle, vm::ptr<CellPngGAMA> gama)
{
	UNIMPLEMENTED_FUNC(cellPngDec);
	return CELL_OK;
}

s32 cellPngDecGetPLTE(CellPngDecMainHandle mainHandle, CellPngDecSubHandle subHandle, vm::ptr<CellPngPLTE> plte)
{
	UNIMPLEMENTED_FUNC(cellPngDec);
	return CELL_OK;
}

s32 cellPngDecGetTextChunk(
	CellPngDecMainHandle mainHandle,
	CellPngDecSubHandle subHandle,
	vm::ptr<u32> textInfoNum,
	vm::ptr<vm::bptr<CellPngTextInfo>> textInfo)
{
	UNIMPLEMENTED_FUNC(cellPngDec);
	return CELL_OK;
}

Module cellPngDec("cellPngDec", []()
{
	REG_FUNC(cellPngDec, cellPngDecGetUnknownChunks);
	REG_FUNC(cellPngDec, cellPngDecClose);
	REG_FUNC(cellPngDec, cellPngDecGetpCAL);
	REG_FUNC(cellPngDec, cellPngDecGetcHRM);
	REG_FUNC(cellPngDec, cellPngDecGetsCAL);
	REG_FUNC(cellPngDec, cellPngDecGetpHYs);
	REG_FUNC(cellPngDec, cellPngDecGetoFFs);
	REG_FUNC(cellPngDec, cellPngDecGetsPLT);
	REG_FUNC(cellPngDec, cellPngDecGetbKGD);
	REG_FUNC(cellPngDec, cellPngDecGettIME);
	REG_FUNC(cellPngDec, cellPngDecGethIST);
	REG_FUNC(cellPngDec, cellPngDecGettRNS);
	REG_FUNC(cellPngDec, cellPngDecGetsBIT);
	REG_FUNC(cellPngDec, cellPngDecGetiCCP);
	REG_FUNC(cellPngDec, cellPngDecGetsRGB);
	REG_FUNC(cellPngDec, cellPngDecGetgAMA);
	REG_FUNC(cellPngDec, cellPngDecGetPLTE);
	REG_FUNC(cellPngDec, cellPngDecGetTextChunk);
	REG_FUNC(cellPngDec, cellPngDecDestroy);
	REG_FUNC(cellPngDec, cellPngDecCreate);
	REG_FUNC(cellPngDec, cellPngDecExtCreate);
	REG_FUNC(cellPngDec, cellPngDecExtSetParameter);
	REG_FUNC(cellPngDec, cellPngDecSetParameter);
	REG_FUNC(cellPngDec, cellPngDecExtReadHeader);
	REG_FUNC(cellPngDec, cellPngDecReadHeader);
	REG_FUNC(cellPngDec, cellPngDecExtOpen);
	REG_FUNC(cellPngDec, cellPngDecOpen);
	REG_FUNC(cellPngDec, cellPngDecExtDecodeData);
	REG_FUNC(cellPngDec, cellPngDecDecodeData);
});
