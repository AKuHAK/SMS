/*
#     ___  _ _      ___
#    |    | | |    |
# ___|    |   | ___|    PS2DEV Open Source Project.
#----------------------------------------------------------
# (c) 2005 Eugene Plotnikov <e-plotnikov@operamail.com>
# Licenced under Academic Free License version 2.0
# Review ps2sdk README & LICENSE files for further details.
#
*/
#include "SMS_Player.h"
#include "SMS_PlayerControl.h"
#include "GS.h"
#include "GUI.h"
#include "IPU.h"
#include "SPU.h"
#include "FileContext.h"
#include "SMS_AudioBuffer.h"
#include "SMS_VideoBuffer.h"
#include "Timer.h"
#include "CDDA.h"
#include "DMA.h"

#include <kernel.h>
#include <malloc.h>
#include <stdio.h>
#include <libpad.h>

#define SMS_VPACKET_QSIZE    384
#define SMS_APACKET_QSIZE    384
#define SMS_VIDEO_QUEUE_SIZE  10
#define SMS_AUDIO_QUEUE_SIZE  10

#define SMS_FLAGS_STOP  0x00000001
#define SMS_FLAGS_PAUSE 0x00000002

static SMS_Player s_Player;

static SMS_AVPacket**    s_VPacketBuffer;
static SMS_AVPacket**    s_APacketBuffer;
static SMS_FrameBuffer** s_VideoBuffer;
static uint8_t**         s_AudioBuffer;

static SMS_RB_CREATE( s_VPacketQueue, SMS_AVPacket*    );
static SMS_RB_CREATE( s_APacketQueue, SMS_AVPacket*    );
static SMS_RB_CREATE( s_VideoQueue,   SMS_FrameBuffer* );
static SMS_RB_CREATE( s_AudioQueue,   uint8_t*         );

extern void* _gp;

static int              s_SemaRPutVideo;
static int              s_SemaDPutVideo;
static int              s_SemaRPutAudio;
static int              s_SemaDPutAudio;
static int              s_SemaPauseAudio;
static int              s_SemaPauseVideo;
static int              s_MainThreadID;
static int              s_VideoRThreadID;
static int              s_VideoDThreadID;
static int              s_AudioRThreadID;
static int              s_AudioDThreadID;
static uint8_t          s_VideoRStack[ 0x10000 ] __attribute__(   (  aligned( 16 )  )   );
static uint8_t          s_VideoDStack[ 0x10000 ] __attribute__(   (  aligned( 16 )  )   );
static uint8_t          s_AudioRStack[ 0x10000 ] __attribute__(   (  aligned( 16 )  )   );
static uint8_t          s_AudioDStack[ 0x10000 ] __attribute__(   (  aligned( 16 )  )   );
static SMS_Codec*       s_pVideoCodec;
static int              s_VideoIdx;
static SMS_FrameBuffer* s_pFrame;
static SMS_Codec*       s_pAudioCodec;
static int              s_AudioIdx;
static SMS_AudioBuffer* s_AudioSamples;
static float            s_AudioTime;
static int              s_nPackets;
static int              s_Flags;

#ifdef LOCK_QUEUES
static int s_SemaPALock;
static int s_SemaPVLock;
static int s_SemaVLock;
static int s_SemaALock;

# define LOCK( s ) WaitSema ( s )
# define UNLOCK( s ) SignalSema ( s )
#else
# define LOCK( s )
# define UNLOCK( s )
#endif  /* LOCK_QUEUES */

static void _draw_text ( char* apStr ) {

 int lWidth = s_Player.m_pGUICtx -> m_pGSCtx -> TextWidth ( apStr, 0 );
 int lX     = ( s_Player.m_pGUICtx -> m_pGSCtx -> m_Width  - lWidth ) / 2;
 int lY     = ( s_Player.m_pGUICtx -> m_pGSCtx -> m_Height -     26 ) / 2;

 if ( s_Player.m_pIPUCtx ) s_Player.m_pIPUCtx -> Sync ();

 s_Player.m_pGUICtx -> m_pGSCtx -> DrawText ( lX, lY, 0, apStr, 0 );

}  /* end _draw_text */

static void _prepare_ipu_context ( int afVideo ) {

 s_Player.m_pGUICtx -> m_pGSCtx -> m_fDblBuf = GS_OFF;
 s_Player.m_pGUICtx -> m_pGSCtx -> m_fZBuf   = GS_ON;

 s_Player.m_pGUICtx -> m_pGSCtx -> ClearScreen (  GS_SETREG_RGBA( 0x00, 0x00, 0x00, 0x00 )  );
 s_Player.m_pGUICtx -> m_pGSCtx -> VSync ();
 s_Player.m_pGUICtx -> m_pGSCtx -> InitScreen ();
 s_Player.m_pGUICtx -> m_pGSCtx -> VSync ();
 s_Player.m_pGUICtx -> m_pGSCtx -> ClearScreen (  GS_SETREG_RGBA( 0x00, 0x00, 0x00, 0x00 )  );

 if ( afVideo )

  s_Player.m_pIPUCtx = IPU_InitContext (
   s_Player.m_pGUICtx -> m_pGSCtx,
   s_Player.m_pCont -> m_pStm[ s_VideoIdx ] -> m_Codec.m_Width,
   s_Player.m_pCont -> m_pStm[ s_VideoIdx ] -> m_Codec.m_Height
  );

}  /* end _prepare_ipu_context */

static void _sms_play_v ( void ) {

 int              lSize;
 SMS_FrameBuffer* lpFrame;
 SMS_AVPacket*    lpPacket = s_Player.m_pCont -> NewPacket ( s_Player.m_pCont );
 float            lFrameRate = ( float )s_Player.m_pCont -> m_pStm[ s_VideoIdx ] -> m_RealFrameRate / ( float )s_Player.m_pCont -> m_pStm[ s_VideoIdx ] -> m_RealFrameRateBase;
 float            lDiff;
 float            lNextFrame = g_Timer;
 float            lFrameTime = 1000.0F / lFrameRate;
 char             lBuff[ 128 ];

 sprintf ( lBuff, "Buffering %s file (video only)...", s_Player.m_pCont -> m_pName  );

 s_Player.m_pGUICtx -> Status ( lBuff );
 s_Player.m_pFileCtx -> Stream ( s_Player.m_pFileCtx, s_Player.m_pFileCtx -> m_CurPos, 384 );

 _prepare_ipu_context ( 1 );

 while ( 1 ) {

  int lButtons = GUI_ReadButtons ();

  if ( lButtons & PAD_SELECT ) {

   lNextFrame = g_Timer;
   _draw_text ( "Pause" );
   GUI_WaitButton ( PAD_START, 0 );

  } else if ( lButtons & PAD_TRIANGLE ) {

   s_Player.m_pIPUCtx -> Sync ();
   _draw_text ( "Stopping" );
   break;

  }  /* end if */

  lSize = s_Player.m_pCont -> ReadPacket ( lpPacket );

  if ( lSize < 0 )

   break;

  else if ( lSize == 0 ) continue;

  if ( lpPacket -> m_StmIdx != s_VideoIdx ) continue;

  if (  s_pVideoCodec -> Decode (
         &s_Player.m_pCont -> m_pStm[ s_VideoIdx ] -> m_Codec, ( void** )&lpFrame, lpPacket -> m_pData, lpPacket -> m_Size
        )
  ) {

   s_Player.m_pIPUCtx -> Sync ();

   lDiff = lNextFrame - g_Timer;

   if ( lDiff > 0.0F ) Timer_Wait ( lDiff );

   s_Player.m_pIPUCtx -> Display ( lpFrame );

  }  /* end if */

  lNextFrame = g_Timer + lFrameTime;

 }  /* end while */

 lpPacket -> Destroy ( lpPacket );

}  /* end _sms_play_v */

static void _sms_play_a ( void ) {

 static SMS_AudioBuffer s_DummyBuffer;

 int           lSize;
 SMS_AVPacket* lpPacket;
 char          lBuff[ 128 ];

 sprintf ( lBuff, "Buffering %s file (audio only)...", s_Player.m_pCont -> m_pName  );

 lpPacket = s_Player.m_pCont -> NewPacket ( s_Player.m_pCont );

 s_AudioSamples = &s_DummyBuffer;

 s_Player.m_pGUICtx -> Status ( lBuff );
 s_Player.m_pFileCtx -> Stream ( s_Player.m_pFileCtx, s_Player.m_pFileCtx -> m_CurPos, 384 );

 _prepare_ipu_context ( 0 );

 s_Player.m_pGUICtx -> m_pGSCtx -> DrawText ( 10, 60, 0, "Audio only", 0 );

 s_Player.m_pSPUCtx = SPU_InitContext (
  s_Player.m_pCont -> m_pStm[ s_AudioIdx ] -> m_Codec.m_Channels,
  s_Player.m_pCont -> m_pStm[ s_AudioIdx ] -> m_Codec.m_SampleRate,
  Index2Volume ( &s_Player )
 );

 if ( s_Player.m_pSPUCtx )

  while ( 1 ) {

   int lButtons = GUI_ReadButtons ();

   if ( lButtons & PAD_SELECT ) {

    _draw_text ( "Pause" );
    s_Player.m_pSPUCtx -> SetVolume ( 0 );
    GUI_WaitButton ( PAD_START, 0 );
    s_Player.m_pSPUCtx -> SetVolume (  Index2Volume ( &s_Player )  );
    s_Player.m_pGUICtx -> m_pGSCtx -> ClearScreen (  GS_SETREG_RGBA( 0x00, 0x00, 0x00, 0x00 )  );
    s_Player.m_pGUICtx -> m_pGSCtx -> DrawText ( 10, 60, 0, "Audio only", 0 );

   } else if ( lButtons & PAD_TRIANGLE ) {

    _draw_text ( "Stopping" );
    break;

   }  /* end if */

   lSize = s_Player.m_pCont -> ReadPacket ( lpPacket );

   if ( lSize < 0 ) break;

   if ( lpPacket -> m_StmIdx != s_AudioIdx ) continue;

   s_AudioSamples -> m_Len = 0;

   do {

    if (  s_pAudioCodec -> Decode (
           &s_Player.m_pCont -> m_pStm[ s_AudioIdx ] -> m_Codec, ( void** )&s_AudioSamples, lpPacket -> m_pData, lpPacket -> m_Size
          )
    ) {

     s_Player.m_pSPUCtx -> PlayPCM ( s_AudioSamples -> m_pOut );

     s_AudioSamples -> Release ();

    } else break;

   } while ( s_AudioSamples -> m_Len > 0 );

  }  /* end while */

  lpPacket -> Destroy ( lpPacket );

}  /* end _sms_play_a */

static void _sms_video_renderer ( void* apParam ) {

 float lDiff;

 while ( 1 ) {

  SleepThread ();

  if (  SMS_RB_EMPTY( s_VideoQueue )  ) break;

  if ( s_Flags & SMS_FLAGS_PAUSE ) {

   _draw_text ( "Pause" );
   WaitSema ( s_SemaPauseVideo );

  }  /* end if */

  LOCK( s_SemaVLock );
   s_pFrame = *SMS_RB_POPSLOT( s_VideoQueue );
   SMS_RB_POPADVANCE( s_VideoQueue );
  UNLOCK( s_SemaVLock );

  lDiff = s_pFrame -> m_PTS - s_AudioTime;

  if ( s_Flags & SMS_FLAGS_STOP ) {

   SignalSema ( s_SemaRPutVideo );
   break;

  }  /* end if */

  if ( lDiff > 20.0F ) Timer_Wait ( lDiff / 4.0F );

  s_Player.m_pIPUCtx -> Sync ();
  s_Player.m_pIPUCtx -> Display ( s_pFrame );

  SignalSema ( s_SemaRPutVideo );

 }  /* end while */

 WakeupThread ( s_MainThreadID );
 ExitDeleteThread ();

}  /* end _sms_video_renderer */

static void _sms_video_decoder ( void* apParam ) {

 SMS_AVPacket*    lpPacket;
 SMS_FrameBuffer* lpFrame;

 while ( 1 ) {

  SleepThread ();

  if (  SMS_RB_EMPTY( s_VPacketQueue )  ) break;

  LOCK( s_SemaPVLock );
   lpPacket = *SMS_RB_POPSLOT( s_VPacketQueue );
   SMS_RB_POPADVANCE( s_VPacketQueue );
  UNLOCK( s_SemaPVLock );

  --s_nPackets;

  if (  s_pVideoCodec -> Decode (
         &s_Player.m_pCont -> m_pStm[ s_VideoIdx ] -> m_Codec, ( void** )&lpFrame, lpPacket -> m_pData, lpPacket -> m_Size
        )
  ) {

   if ( s_Flags & SMS_FLAGS_STOP ) {

    lpPacket -> Destroy ( lpPacket );
    break;

   }  /* end if */

   WaitSema ( s_SemaRPutVideo );

   lpFrame -> m_PTS = lpPacket -> m_PTS;

   LOCK( s_SemaVLock );
    *SMS_RB_PUSHSLOT( s_VideoQueue ) = lpFrame;
    SMS_RB_PUSHADVANCE( s_VideoQueue );
   UNLOCK( s_SemaVLock );

   WakeupThread ( s_VideoRThreadID );

  }  /* end if */

  lpPacket -> Destroy ( lpPacket );

  SignalSema ( s_SemaDPutVideo );

 }  /* end while */

 WakeupThread ( s_MainThreadID );
 ExitDeleteThread ();

}  /* end _sms_video_decoder */

static void _sms_audio_renderer ( void* apParam ) {

 uint8_t* lpSamples;
 uint32_t lnChannels = s_Player.m_pCont -> m_pStm[ s_AudioIdx ] -> m_Codec.m_Channels;
 uint32_t lBPS       = s_Player.m_pCont -> m_pStm[ s_AudioIdx ] -> m_Codec.m_BitsPerSample;
 uint32_t lSPS       = s_Player.m_pCont -> m_pStm[ s_AudioIdx ] -> m_Codec.m_SampleRate;
 uint32_t lPlayed    = 0;

 if ( !lBPS ) lBPS = 16;

 s_AudioTime = 0.0F;

 while ( 1 ) {

  SleepThread ();

  if (  SMS_RB_EMPTY( s_AudioQueue )  ) {

   s_Player.m_pSPUCtx -> SetVolume (  Index2Volume ( &s_Player )  );
   break;

  }  /* end if */

  if ( s_Flags & SMS_FLAGS_PAUSE ) {

   s_Player.m_pSPUCtx -> SetVolume ( 0 );
   WaitSema ( s_SemaPauseAudio );
   s_Player.m_pSPUCtx -> SetVolume (  Index2Volume ( &s_Player )  );

  }  /* end if */

  LOCK( s_SemaALock );
   lpSamples = *SMS_RB_POPSLOT( s_AudioQueue );
   SMS_RB_POPADVANCE( s_AudioQueue );
  UNLOCK( s_SemaALock );

  SignalSema ( s_SemaRPutAudio );

  if ( s_Flags & SMS_FLAGS_STOP ) {

   s_AudioSamples -> Release ();
   break;

  }  /* end if */

  lPlayed    += *( int* )lpSamples;
  s_AudioTime = (  lPlayed * (  1000.0F / ( lBPS * lnChannels / 8 )  )   ) / ( float )lSPS;

  s_Player.m_pSPUCtx -> PlayPCM ( lpSamples );

  s_AudioSamples -> Release ();

 }  /* end while */

 WakeupThread ( s_MainThreadID );
 ExitDeleteThread ();

}  /* end _sms_audio_renderer */

static void _sms_audio_decoder ( void* apParam ) {

 static SMS_AudioBuffer s_DummyBuffer;

 SMS_AVPacket* lpPacket;

 s_AudioSamples = &s_DummyBuffer;

 while ( 1 ) {

  SleepThread ();

  if (  SMS_RB_EMPTY( s_APacketQueue )  ) break;

  LOCK( s_SemaPALock );
   lpPacket = *SMS_RB_POPSLOT( s_APacketQueue );
   SMS_RB_POPADVANCE( s_APacketQueue );
  UNLOCK( s_SemaPALock );

  --s_nPackets;

  s_AudioSamples -> m_Len = 0;

  do {

   if (  s_pAudioCodec -> Decode (
          &s_Player.m_pCont -> m_pStm[ s_AudioIdx ] -> m_Codec, ( void** )&s_AudioSamples, lpPacket -> m_pData, lpPacket -> m_Size
         )
   ) {

    if ( s_Flags & SMS_FLAGS_STOP ) {

     lpPacket -> Destroy ( lpPacket );
     goto end;

    }  /* end if */

    WaitSema ( s_SemaRPutAudio );

    LOCK( s_SemaALock );
     *SMS_RB_PUSHSLOT( s_AudioQueue ) = s_AudioSamples -> m_pOut;
     SMS_RB_PUSHADVANCE( s_AudioQueue );
    UNLOCK( s_SemaALock );

    WakeupThread ( s_AudioRThreadID );

   } else break;

  } while ( s_AudioSamples -> m_Len > 0 );

  lpPacket -> Destroy ( lpPacket );

  SignalSema ( s_SemaDPutAudio );

 }  /* end while */
end:
 WakeupThread ( s_MainThreadID );
 ExitDeleteThread ();

}  /* end _sms_audio_decoder */

static void _sms_play_a_v ( void ) {

 int           lSize;
 SMS_AVPacket* lpPacket;
 char          lBuff[ 128 ];

 sprintf ( lBuff, "Buffering %s file...", s_Player.m_pCont -> m_pName  );

 s_nPackets = 0;

 s_Player.m_pGUICtx -> Status ( lBuff );
 s_Player.m_pFileCtx -> Stream ( s_Player.m_pFileCtx, s_Player.m_pFileCtx -> m_CurPos, 768 );

 s_Player.m_pSPUCtx = SPU_InitContext (
  s_Player.m_pCont -> m_pStm[ s_AudioIdx ] -> m_Codec.m_Channels,
  s_Player.m_pCont -> m_pStm[ s_AudioIdx ] -> m_Codec.m_SampleRate,
  Index2Volume ( &s_Player )
 );

 _prepare_ipu_context ( 1 );

 if ( s_Player.m_pSPUCtx && s_Player.m_pIPUCtx ) {

  uint64_t lNextTime = 0;

  InitPlayerControl ( &s_Player );

  while ( 1 ) {

   uint32_t lButtons = GUI_ReadButtons ();

   if ( lButtons ) {

    if ( g_Timer <= lNextTime ) goto skip;

    lNextTime = g_Timer + 200;

    if ( lButtons & PAD_SELECT ) {

     unsigned long int lBtn;

     s_Flags |= SMS_FLAGS_PAUSE;

     while ( 1 ) {

      lBtn = GUI_WaitButton ( PAD_START | PAD_SELECT, 200 );

      if ( lBtn == PAD_START ) break;

      if ( lBtn == PAD_SELECT ) {

       s_Player.m_pIPUCtx -> Sync ();
       s_Player.m_pIPUCtx -> Display ( s_pFrame );

      }  /* end if */

     }  /* end while */

     s_Flags &= ~SMS_FLAGS_PAUSE;
     SignalSema ( s_SemaPauseAudio );
     SignalSema ( s_SemaPauseVideo );

    } else if (  lButtons & PAD_TRIANGLE && *( int* )&s_AudioTime  ) {

     int           i;
     SMS_AVPacket* lpPacket;

     s_Flags |= SMS_FLAGS_STOP;

     WakeupThread ( s_VideoRThreadID );
     WakeupThread ( s_AudioRThreadID );
     WakeupThread ( s_VideoDThreadID );
     WakeupThread ( s_AudioDThreadID );

     for ( i = 0; i < 4; ++i ) SleepThread ();

     while (  !SMS_RB_EMPTY( s_VPacketQueue )  ) {

      lpPacket = *SMS_RB_POPSLOT( s_VPacketQueue );
      lpPacket -> Destroy ( lpPacket );
      SMS_RB_POPADVANCE( s_VPacketQueue );

     }  /* end while */

     while (  !SMS_RB_EMPTY( s_APacketQueue )  ) {

      lpPacket = *SMS_RB_POPSLOT( s_APacketQueue );
      lpPacket -> Destroy ( lpPacket );
      SMS_RB_POPADVANCE( s_APacketQueue );

     }  /* end while */

     _draw_text ( "Stopping" );

     break;

    } else if ( lButtons == PAD_UP ) {

     AdjustVolume ( &s_Player, 1 );

    } if ( lButtons == PAD_DOWN ) {

     AdjustVolume ( &s_Player, -1 );

    }  /* end if */

   }  /* end if */
skip:
   g_CDDASpeed = s_nPackets < 128 ? 4 : 3;

   lpPacket = s_Player.m_pCont -> NewPacket ( s_Player.m_pCont );
nextPacket:
   lSize = s_Player.m_pCont -> ReadPacket ( lpPacket );

   if ( lSize < 0 ) {

    lpPacket -> Destroy ( lpPacket );
    break;

   } else if ( lSize == 0 ) continue;

   if ( lpPacket -> m_StmIdx == s_VideoIdx ) {

    WaitSema ( s_SemaDPutVideo );

    LOCK( s_SemaPVLock );
     *SMS_RB_PUSHSLOT( s_VPacketQueue ) = lpPacket;
     SMS_RB_PUSHADVANCE( s_VPacketQueue );
    UNLOCK( s_SemaPVLock );

    ++s_nPackets;

    WakeupThread ( s_VideoDThreadID );

   } else if ( lpPacket -> m_StmIdx == s_AudioIdx ) {

    WaitSema ( s_SemaDPutAudio );

    LOCK( s_SemaPALock );
     *SMS_RB_PUSHSLOT( s_APacketQueue ) = lpPacket;
     SMS_RB_PUSHADVANCE( s_APacketQueue );
    UNLOCK( s_SemaPALock );

    ++s_nPackets;

    WakeupThread ( s_AudioDThreadID );

   } else goto nextPacket;

  }  /* end while */

 } else if ( s_Player.m_pIPUCtx )

  _sms_play_v ();

 else if ( s_Player.m_pSPUCtx ) _sms_play_a ();

}  /* end _sms_play_a_v */

static void _Destroy ( void ) {

 DiskType lType;

 if ( s_VideoBuffer ) {

  if (  !( s_Flags & SMS_FLAGS_STOP )  ) {

   WakeupThread ( s_VideoDThreadID );
   SleepThread ();

  }  /* end if */

  DeleteSema ( s_SemaDPutVideo );
  free ( s_VPacketBuffer );

  if (  !( s_Flags & SMS_FLAGS_STOP )  ) {

   WakeupThread ( s_VideoRThreadID );  
   SleepThread ();

  }  /* end if */

  DeleteSema ( s_SemaRPutVideo  );
  DeleteSema ( s_SemaPauseVideo );
  free ( s_VideoBuffer );
#ifdef LOCK_QUEUES
  DeleteSema ( s_SemaPVLock );
  DeleteSema ( s_SemaVLock  );
#endif  /* LOCK_QUEUES */
 }  /* end if */

 if ( s_AudioBuffer ) {

  if (  !( s_Flags & SMS_FLAGS_STOP )  ) {

   WakeupThread ( s_AudioDThreadID );
   SleepThread ();

  }  /* end if */

  DeleteSema ( s_SemaDPutAudio );
  free ( s_APacketBuffer );

  if (  !( s_Flags & SMS_FLAGS_STOP )  ) {

   WakeupThread ( s_AudioRThreadID );
   SleepThread ();

  }  /* end if */

  DeleteSema ( s_SemaRPutAudio );
  DeleteSema ( s_SemaPauseAudio );
  free ( s_AudioBuffer );
#ifdef LOCK_QUEUES
  DeleteSema ( s_SemaPALock );
  DeleteSema ( s_SemaALock  );
#endif  /* LOCK_QUEUES */
 }  /* end if */

 if ( s_Player.m_pSPUCtx ) {

  s_Player.m_pSPUCtx -> Destroy ();
  s_Player.m_pSPUCtx = NULL;

 }  /* end if */

 if ( s_Player.m_pIPUCtx ) {

  s_Player.m_pIPUCtx -> Sync    ();
  s_Player.m_pIPUCtx -> Destroy ();
  s_Player.m_pIPUCtx = NULL;

 }  /* end if */

 lType = CDDA_DiskType ();

 if (  lType == DiskType_CD  ||
       lType == DiskType_DVD ||
       lType == DiskType_CDDA
 ) {

  CDDA_Synchronize ();
  CDDA_Stop        ();
  CDDA_Synchronize ();

 }  /* end if */

 s_Player.m_pCont -> Destroy ( s_Player.m_pCont );
 s_Player.m_pCont = NULL;

}  /* end _Destroy */

SMS_Player* SMS_InitPlayer ( FileContext* apFileCtx, GUIContext* apGUICtx ) {

 s_VPacketBuffer = NULL;
 s_APacketBuffer = NULL;
 s_VideoBuffer   = NULL;
 s_AudioBuffer   = NULL;
 s_Flags         =    0;

 if ( !s_Player.m_Volume ) s_Player.m_Volume = 12;

 apGUICtx -> Status ( "Detecting file format..." );

 s_Player.m_pCont = SMS_GetContainer ( apFileCtx );
 s_MainThreadID   = GetThreadId ();

 if ( s_Player.m_pCont != NULL ) {

  int i;

  s_VideoIdx = 0xFFFFFFFF;
  s_AudioIdx = 0xFFFFFFFF;

  for ( i = 0; i < s_Player.m_pCont -> m_nStm; ++i )

   if ( s_Player.m_pCont -> m_pStm[ i ] -> m_Codec.m_Type == SMS_CodecTypeVideo && s_VideoIdx == 0xFFFFFFFF ) {

    SMS_CodecOpen ( &s_Player.m_pCont -> m_pStm[ i ] -> m_Codec );

    if ( s_Player.m_pCont -> m_pStm[ i ] -> m_Codec.m_pCodec ) {

     s_pVideoCodec = s_Player.m_pCont -> m_pStm[ i ] -> m_Codec.m_pCodec;
     s_pVideoCodec -> Init ( &s_Player.m_pCont -> m_pStm[ i ] -> m_Codec );

     s_VideoIdx = i;

    }  /* end if */

   } else if ( s_Player.m_pCont -> m_pStm[ i ] -> m_Codec.m_Type == SMS_CodecTypeAudio && s_AudioIdx == 0xFFFFFFFF ) {

    SMS_CodecOpen ( &s_Player.m_pCont -> m_pStm[ i ] -> m_Codec );

    if ( s_Player.m_pCont -> m_pStm[ i ] -> m_Codec.m_pCodec ) {

     s_pAudioCodec = s_Player.m_pCont -> m_pStm[ i ] -> m_Codec.m_pCodec;

     if ( s_Player.m_pCont -> m_pStm[ i ] -> m_Codec.m_ID == SMS_CodecID_AC3 ) s_Player.m_pCont -> m_pStm[ i ] -> m_Codec.m_Channels = 2;

     s_pAudioCodec -> Init ( &s_Player.m_pCont -> m_pStm[ i ] -> m_Codec );

     s_AudioIdx = i;

    }  /* end if */

   }  /* end if */

  s_Player.m_pGUICtx  = apGUICtx;
  s_Player.m_pFileCtx = apFileCtx;
  s_Player.Destroy    = _Destroy;

  if ( s_AudioIdx != 0xFFFFFFFF && s_VideoIdx != 0xFFFFFFFF ) {

   ee_sema_t   lSema;
   ee_thread_t lThread;
   ee_thread_t lCurrentThread;

   ReferThreadStatus ( s_MainThreadID, &lCurrentThread );

   s_Player.Play = _sms_play_a_v;

   lThread.stack_size       = sizeof ( s_VideoRStack );
   lThread.stack            = s_VideoRStack;
   lThread.initial_priority = lCurrentThread.current_priority;
   lThread.gp_reg           = &_gp;
   lThread.func             = _sms_video_renderer;
   StartThread (  s_VideoRThreadID = CreateThread ( &lThread ), s_Player.m_pCont  );

   lThread.stack_size       = sizeof ( s_VideoDStack );
   lThread.stack            = s_VideoDStack;
   lThread.initial_priority = lCurrentThread.current_priority;
   lThread.gp_reg           = &_gp;
   lThread.func             = _sms_video_decoder;
   StartThread (  s_VideoDThreadID = CreateThread ( &lThread ), s_Player.m_pCont  );

   lThread.stack_size       = sizeof ( s_AudioRStack );
   lThread.stack            = s_AudioRStack;
   lThread.initial_priority = lCurrentThread.current_priority;
   lThread.gp_reg           = &_gp;
   lThread.func             = _sms_audio_renderer;
   StartThread (  s_AudioRThreadID = CreateThread ( &lThread ), s_Player.m_pCont  );

   lThread.stack_size       = sizeof ( s_AudioDStack );
   lThread.stack            = s_AudioDStack;
   lThread.initial_priority = lCurrentThread.current_priority;
   lThread.gp_reg           = &_gp;
   lThread.func             = _sms_audio_decoder;
   StartThread (  s_AudioDThreadID = CreateThread ( &lThread ), s_Player.m_pCont  );

   s_VPacketBuffer = ( SMS_AVPacket**    )calloc (  SMS_VPACKET_QSIZE,    sizeof ( SMS_AVPacket*    )  );
   s_APacketBuffer = ( SMS_AVPacket**    )calloc (  SMS_APACKET_QSIZE,    sizeof ( SMS_AVPacket*    )  );
   s_VideoBuffer   = ( SMS_FrameBuffer** )calloc (  SMS_VIDEO_QUEUE_SIZE, sizeof ( SMS_FrameBuffer* )  );
   s_AudioBuffer   = ( uint8_t**         )calloc (  SMS_AUDIO_QUEUE_SIZE, sizeof ( uint8_t*         )  );

   SMS_RB_INIT( s_VPacketQueue, s_VPacketBuffer, SMS_VPACKET_QSIZE    );
   SMS_RB_INIT( s_APacketQueue, s_APacketBuffer, SMS_APACKET_QSIZE    );
   SMS_RB_INIT( s_VideoQueue,   s_VideoBuffer,   SMS_VIDEO_QUEUE_SIZE );
   SMS_RB_INIT( s_AudioQueue,   s_AudioBuffer,   SMS_AUDIO_QUEUE_SIZE );

   lSema.init_count =
   lSema.max_count  = SMS_VPACKET_QSIZE - 1;
   s_SemaDPutVideo = CreateSema ( &lSema );

   lSema.init_count =
   lSema.max_count  = SMS_APACKET_QSIZE - 1;
   s_SemaDPutAudio = CreateSema ( &lSema );

   lSema.init_count = 
   lSema.max_count  = SMS_VIDEO_QUEUE_SIZE - 1;
   s_SemaRPutVideo = CreateSema ( &lSema );

   lSema.init_count =
   lSema.max_count  = SMS_AUDIO_QUEUE_SIZE - 1;
   s_SemaRPutAudio = CreateSema ( &lSema );

   lSema.init_count = 0;
   lSema.max_count  = 1;
   s_SemaPauseAudio = CreateSema ( &lSema );
   s_SemaPauseVideo = CreateSema ( &lSema );
#ifdef LOCK_QUEUES
   lSema.init_count = 1;
   s_SemaPALock = CreateSema ( &lSema );
   s_SemaPVLock = CreateSema ( &lSema );
   s_SemaVLock  = CreateSema ( &lSema );
   s_SemaALock  = CreateSema ( &lSema );
#endif  /* end LOCK_QUEUES */
  } else if ( s_VideoIdx != 0xFFFFFFFF ) {

   s_Player.Play = _sms_play_v;

  } else if ( s_AudioIdx != 0xFFFFFFFF ) {

   s_Player.Play = _sms_play_a;

  } else s_Player.Play = NULL;

 } else apFileCtx -> Destroy ( apFileCtx );

 return s_Player.m_pCont ? &s_Player : NULL;

}  /* end SMS_AVIInitPlayer */
