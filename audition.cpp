#include <windows.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include "filters.h"
#include "dbglog.h"
#include "win32/winspk.h"
#include "registry.h"
#include "resource.h"

#include "parsers/file_parser.h"
#include "parsers/ac3/ac3_header.h"
#include "parsers/dts/dts_header.h"
#include "filters/decoder_graph.h"

#include "parsers/ac3/ac3_enc.h"
#include "filters/convert.h"
#include "filter_graph.h"
#include "sink/sink_raw.h"



#ifndef OUTPUT_FORMAT
#define OUTPUT_FORMAT FORMAT_PCM16
#endif

#ifndef REG_KEY
#define REG_KEY "Software\\AC3Filter\\Audition"
#endif



///////////////////////////////////////////////////////////////////////////////
// CoolInput
// Input class. File open, info and read operations.
///////////////////////////////////////////////////////////////////////////////



class CoolInput
{
protected:
  Speakers file_spk;
  Chunk out_chunk;
  FileParser file;
  DecoderGraph dec;
  char info_buf[1024];

public:
  CoolInput()
  {}

  ~CoolInput()
  {
    close();
  }

  /////////////////////////////////////////////////////////
  // File operations

  bool open(const char *filename)
  {
    close();

    // Set output format of the decoder
    if (!dec.set_user(Speakers(OUTPUT_FORMAT, 0, 0)))
      return false;

    // Try to open the file and gather statistics
    if (file.open(filename, &ac3_header, 1000000))
      if (!file.stats())
        if (file.open(filename, &dts_header, 1000000))
          if (!file.stats())
          {
            close();
            return false;
          }

    // Determine file format and fill info
    if (!file.load_frame())
    {
      close();
      return false;
    }
    file_spk = file.get_spk();
    file.stream_info(info_buf, array_size(info_buf));

    // Reset everything
    file.seek(0);
    dec.reset();
    out_chunk.set_empty(spk_unknown);

    return true;
  }

  bool is_open() const
  { 
    return file.is_open(); 
  }

  void close()
  {
    // Reset everything
    file.close();
    dec.reset();
    file_spk = spk_unknown;
    out_chunk.set_empty(spk_unknown);
  }

  /////////////////////////////////////////////////////////
  // File info

  // Output format
  Speakers spk() const { Speakers result = file_spk; result.format = OUTPUT_FORMAT; return result; }

  // Number of channels
  int nch() const { return file_spk.nch(); }

  // Sample rate
  int sample_rate() const { return file_spk.sample_rate; }

  // Bits per sample
  int bps() const { return sample_size(OUTPUT_FORMAT) * 8; }

  // Size of output data (after decoding)
  long get_file_size()
  {
    if (!file.is_open()) return 0;
    return (long)(file.get_size(FileParser::time) * file_spk.sample_rate * file_spk.nch() * sample_size(OUTPUT_FORMAT));
  }

  // file description
  int info(char *buf)
  {
    strcpy(buf, info_buf);
    return strlen(info_buf);
  }

  /////////////////////////////////////////////////////////
  // Read and decode

  size_t read(uint8_t *buf, size_t size)
  {
    if (!file.is_open() || file.eof())
      return 0;

    size_t out_size = 0;

    while (1)
    {
      if (out_chunk.size)
      {
        size_t copy_size = MIN(out_chunk.size, size);
        memcpy(buf, out_chunk.rawdata, copy_size);
        buf += copy_size;
        out_chunk.drop(copy_size);
        size -= copy_size;
        out_size += copy_size;
        if (!size)
          return out_size;
      }

      if (!dec.is_empty())
      {
        if (!dec.get_chunk(&out_chunk)) return 0;
        continue;
      }

      if (file.eof())
        return out_size;

      if (file.load_frame())
      {

#ifdef _DEBUG
        double q = 1.0;
        for (int i = 0; i < 15000; i++)
          q *= M_PI;
#endif
        Chunk chunk;
        chunk.set_rawdata(file.get_spk(), file.get_frame(), file.get_frame_size());
        if (!dec.process(&chunk)) return 0;
        continue;
      }
    }


  }
};



///////////////////////////////////////////////////////////////////////////////
// CoolOutput
// Output class. File open, write operations.
///////////////////////////////////////////////////////////////////////////////

class CoolOutput
{
protected:
  Speakers  spk;
  Converter conv;
  AC3Enc    enc;
  RAWSink   sink;
  FilterChain chain;

public:
  CoolOutput(): conv(AC3_FRAME_SAMPLES)
  {
    chain.add_back(&conv, "Converter");
    chain.add_back(&enc,  "Encoder");
    conv.set_format(FORMAT_LINEAR);
    conv.set_order(win_order);
  }

  ~CoolOutput()
  {
    close();
  }


  bool open(const char *_filename, Speakers _spk, int bitrate)
  {
    close();

    // input format ofr encoder
    Speakers enc_spk = _spk;
    enc_spk.format = FORMAT_LINEAR;
    enc_spk.level = 1.0;

    // setup encoder and open output
    if (!enc.set_bitrate(bitrate) || !enc.set_input(enc_spk)) return false;
    if (!sink.open(_filename)) return false;

    spk = _spk;
    return true;
  }

  bool is_open() const
  {
    return sink.is_open();
  }

  void close()
  {
    // close and reset everything
    spk = spk_unknown;
    sink.close();
    chain.reset();
  }

  int write(uint8_t *buf, size_t size)
  {
    Chunk chunk;
    chunk.set_rawdata(spk, buf, size);
    if (!chain.process(&chunk)) return 0;

    size_t out_size = 0;
    while (!chain.is_empty())
    {
#ifdef _DEBUG
        double q = 1.0;
        for (int i = 0; i < 15000; i++)
          q *= M_PI;
#endif
      if (!chain.get_chunk(&chunk)) return 0;
      if (!sink.process(&chunk)) return 0;
      out_size += chunk.size;
    }
    return out_size;
  }

};



///////////////////////////////////////////////////////////////////////////////
// Common filter functions
///////////////////////////////////////////////////////////////////////////////

__declspec(dllexport) short FAR PASCAL QueryCoolFilter(COOLQUERY far * cq)
{
  dbglog("QueryCoolFilter");

  lstrcpy(cq->szName, "AC3/DTS");
  lstrcpy(cq->szCopyright, "AC3/DTS");
  lstrcpy(cq->szExt, "AC3");
  lstrcpy(cq->szExt2, "DTS");

	cq->dwFlags = QF_CANSAVE | QF_CANLOAD | QF_HASOPTIONSBOX | QF_NOHEADER;
  cq->lChunkSize = 16384;

  cq->Stereo16 = R_11025 | R_22050 | R_44100 | R_48000;
  cq->Stereo24 = R_11025 | R_22050 | R_44100 | R_48000;
  cq->Stereo32 = R_11025 | R_22050 | R_44100 | R_48000;
  cq->Mono16   = R_11025 | R_22050 | R_44100 | R_48000;
  cq->Mono24   = R_11025 | R_22050 | R_44100 | R_48000;
  cq->Mono32   = R_11025 | R_22050 | R_44100 | R_48000;
  cq->Quad32   = R_11025 | R_22050 | R_44100 | R_48000;
  cq->Quad16   = R_11025 | R_22050 | R_44100 | R_48000;
  return C_VALIDLIBRARY;
}

__declspec(dllexport) BOOL FAR PASCAL FilterUnderstandsFormat(LPSTR filename)
{ 
  dbglog("FilterUnderstandsFormat(%s)", filename);

  FileParser file;
  const HeaderParser *parser_list[] = { &ac3_header, &dts_header };
  MultiHeader multi_parser(parser_list, array_size(parser_list));

  if (file.open(filename, &multi_parser, 1000000))
    if (file.load_frame())
      return TRUE;

  return FALSE;
}



///////////////////////////////////////////////////////////////////////////////
// Read functions
///////////////////////////////////////////////////////////////////////////////

__declspec(dllexport) HANDLE FAR PASCAL
OpenFilterInput(LPSTR lpstrFilename, long far *lSamprate, WORD far *wBitsPerSample, WORD far *wChannels, HWND hWnd, long far *lChunkSize)
{
  dbglog("OpenFilterInput(%s)", lpstrFilename);

  CoolInput *input = new CoolInput();
  if (!input)
    return 0;

  if (!input->open(lpstrFilename))
  {
    delete input;
    return 0;
  }

  if (lSamprate)      *lSamprate = input->sample_rate();
  if (wBitsPerSample) *wBitsPerSample = input->bps();
  if (wChannels)      *wChannels = input->nch();

  // buffer size for 8K samples
  if (lChunkSize)     *lChunkSize = 8192 * input->bps() * input->nch() / 8;

  return input;
}

__declspec(dllexport) void FAR PASCAL CloseFilterInput(HANDLE hInput)
{
  dbglog("CloseFilterInput");
  if (!hInput) return;

  CoolInput *input = (CoolInput *)hInput;
  delete input;
}

__declspec(dllexport) long FAR PASCAL FilterGetFileSize(HANDLE hInput)
{
  dbglog("FilterGetFileSize");
  if (!hInput) return 0;

  CoolInput *input = (CoolInput *)hInput;
  return input->get_file_size();
}

__declspec(dllexport) DWORD FAR PASCAL ReadFilterInput(HANDLE hInput, unsigned char far *buf, long lBytes)
{
  dbglog("ReadFilterInput(%i bytes)", lBytes);
  if (!hInput) return 0;

  CoolInput *input = (CoolInput *)hInput;
  return input->read(buf, lBytes);
}



///////////////////////////////////////////////////////////////////////////////
// Write functions
///////////////////////////////////////////////////////////////////////////////

Speakers make_spk(int bps, int nch, int sample_rate)
{
  int format;
  switch (bps)
  {
    case 16: format = FORMAT_PCM16; break;
    case 24: format = FORMAT_PCM24; break;
    case 32: format = FORMAT_PCM32; break;
    default: return spk_unknown;
  }

  int mask;
  switch (nch)
  {
    case 1: mask = MODE_MONO; break;
    case 2: mask = MODE_STEREO; break;
    case 4: mask = MODE_QUADRO; break;
    case 5: mask = MODE_3_2; break;
    case 6: mask = MODE_5_1; break;
    default: return spk_unknown;
  }

  return Speakers(format, mask, sample_rate);
}

__declspec(dllexport) HANDLE FAR PASCAL OpenFilterOutput(LPSTR lpstrFilename, long lSamprate, WORD wBitsPerSample, WORD wChannels, long lSize, long far *lpChunkSize, DWORD dwOptions)
{
  dbglog("OpenFilterOutput(%s, %ich %iHz %ibps)", lpstrFilename, wChannels, lSamprate, wBitsPerSample);

  Speakers spk = make_spk(wBitsPerSample, wChannels, lSamprate);
  if (spk.is_unknown())
    return 0;

  int bitrate = 448000;
  RegistryKey reg(REG_KEY);
  reg.get_int32("bitrate", bitrate);

  CoolOutput *output = new CoolOutput();
  if (!output)
    return 0;

  if (!output->open(lpstrFilename, spk, bitrate))
  {
    delete output;
    return 0;
  }

  // buffer size for 8K samples
  if (lpChunkSize) *lpChunkSize = 8192 * wBitsPerSample * wChannels / 8;

  return output;
}

__declspec(dllexport) void FAR PASCAL CloseFilterOutput(HANDLE hOutput)
{
  dbglog("CloseFilterOutput");
  if (!hOutput) return;

  CoolOutput *output = (CoolOutput *)hOutput;
  delete output;
}

__declspec(dllexport) DWORD FAR PASCAL WriteFilterOutput(HANDLE hOutput, unsigned char far *buf, long lBytes)
{
  dbglog("WriteFilterOutput");
  if (!hOutput) return 0;

  CoolOutput *output = (CoolOutput *)hOutput;
  return output->write(buf, lBytes);
}

///////////////////////////////////////////////////////////////////////////////
// Options
///////////////////////////////////////////////////////////////////////////////

__declspec(dllexport) DWORD FAR PASCAL FilterGetOptions(HWND hWnd, HINSTANCE hInst, long lSamprate, WORD wChannels, WORD wBitsPerSample, DWORD dwOptions)
{
  dbglog("FilterGetOptions");
  
  int retval = 0;
  FARPROC lpfnDIALOGMsgProc;
  lpfnDIALOGMsgProc = GetProcAddress(hInst, (LPCSTR)MAKELONG(20,0));

  // sample rate: 18bit (up to 256kHz)
  // bps: 6bit (up to 64bit)
  // channels: 4bit (up to 16 channels)
  dwOptions = lSamprate | (wBitsPerSample << 18) | (wChannels << 24);
  
  return DialogBoxParam(hInst, MAKEINTRESOURCE(IDD_CONFIG), hWnd, (DLGPROC)lpfnDIALOGMsgProc, dwOptions);
}

///////////////////////////////////////////////////////////////////////////////

__declspec(dllexport) DWORD FAR PASCAL FilterOptions(HANDLE hInput)
{
  dbglog("FilterOptions");
  if (!hInput) return 0;

  CoolInput *input = (CoolInput *)hInput;
  return 0;
}

__declspec(dllexport) DWORD FAR PASCAL FilterOptionsString(HANDLE hInput, LPSTR szString)
{
  dbglog("FilterOptionsString");
  if (!hInput) return 0;

  CoolInput *input = (CoolInput *)hInput;
  input->info(szString);
  return 0;
}

__declspec(dllexport) BOOL FAR PASCAL DIALOGMsgProc(HWND hWndDlg, UINT Message, WPARAM wParam, LPARAM lParam)
{
  switch (Message)
  {
  case WM_INITDIALOG:
  {
    int i;
    char buf[128];
    const int bitrate_tbl[19] = 
    {
       32000,  40000,  48000,  56000,  64000,  80000,  96000, 112000, 128000, 
      160000, 192000, 224000, 256000, 320000, 384000, 448000, 512000, 576000, 640000 
    };

    // read bitrate
    int bitrate = 448000;
    RegistryKey reg(REG_KEY);
    reg.get_int32("bitrate", bitrate);

    // verify bitrate
    for (i = 0; i < array_size(bitrate_tbl); i++)
      if (bitrate_tbl[i] == bitrate)
        break;
    if (i == array_size(bitrate_tbl))
      bitrate = 448000;

    // fill combo box
    for (i = 0; i < array_size(bitrate_tbl); i++)
    {
      int cb_index = SendDlgItemMessage(hWndDlg, IDC_CMB_BITRATE, CB_ADDSTRING, 0, (LONG)itoa(bitrate_tbl[i], buf, 10));
      SendDlgItemMessage(hWndDlg, IDC_CMB_BITRATE, CB_SETITEMDATA, cb_index, bitrate_tbl[i]);
      if (bitrate_tbl[i] == bitrate)
        SendDlgItemMessage(hWndDlg, IDC_CMB_BITRATE, CB_SETCURSEL, cb_index, 0);
    }

    // input format
    int sample_rate = lParam & 0x3ffff;
    int bps = (lParam >> 18) & 0x3f;
    int nch = (lParam >> 24) & 0xf;
    sprintf(buf, "%ich %iHz %ibit", nch, sample_rate, bps);
    SetDlgItemText(hWndDlg, IDC_EDT_FORMAT, buf);

    return TRUE;
  }

  case WM_CLOSE:
    PostMessage(hWndDlg, WM_COMMAND, IDCANCEL, 0);
    return TRUE;

  case WM_COMMAND:
    switch (LOWORD(wParam))
    {
      case IDOK:
      {
        int bitrate = 448000;
        int cb_index = SendDlgItemMessage(hWndDlg, IDC_CMB_BITRATE, CB_GETCURSEL, 0, 0);
        if (cb_index != CB_ERR)
          bitrate = SendDlgItemMessage(hWndDlg, IDC_CMB_BITRATE, CB_GETITEMDATA, cb_index, 0);

        RegistryKey reg;
        reg.create_key(REG_KEY);
        reg.set_int32("bitrate", bitrate);

        EndDialog(hWndDlg, bitrate);
        return TRUE; 
      }

      case IDCANCEL:
        EndDialog(hWndDlg, 0);
        break;
    }
  }

  return FALSE;
}



///////////////////////////////////////////////////////////////////////////////
// DllMain
///////////////////////////////////////////////////////////////////////////////



BOOL WINAPI DllMain (HANDLE hModule, DWORD fdwReason, LPVOID lpReserved)
{
  switch (fdwReason)
  {
  case DLL_PROCESS_ATTACH:
    /* 
    Code from LibMain inserted here.  Return TRUE to keep the
    DLL loaded or return FALSE to fail loading the DLL.

    You may have to modify the code in your original LibMain to
    account for the fact that it may be called more than once.
    You will get one DLL_PROCESS_ATTACH for each process that
    loads the DLL. This is different from LibMain which gets
    called only once when the DLL is loaded. The only time this
    is critical is when you are using shared data sections.
    If you are using shared data sections for statically
    allocated data, you will need to be careful to initialize it
    only once. Check your code carefully.

    Certain one-time initializations may now need to be done for
    each process that attaches. You may also not need code from
    your original LibMain because the operating system may now
    be doing it for you.
    */
    break;

  case DLL_THREAD_ATTACH:
    /* 
    Called each time a thread is created in a process that has
    already loaded (attached to) this DLL. Does not get called
    for each thread that exists in the process before it loaded
    the DLL.

    Do thread-specific initialization here.
    */
    break;

  case DLL_THREAD_DETACH:
    /* 
    Same as above, but called when a thread in the process
    exits.

    Do thread-specific cleanup here.
    */
    break;

  case DLL_PROCESS_DETACH:
    /* 
    Code from _WEP inserted here.  This code may (like the
    LibMain) not be necessary.  Check to make certain that the
    operating system is not doing it for you.
    */
    break;
  }
 
  /* 
  The return value is only used for DLL_PROCESS_ATTACH; all other
  conditions are ignored.  
  */
  return TRUE; // successful DLL_PROCESS_ATTACH
}
