/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE OggVorbis SOFTWARE CODEC SOURCE CODE.   *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE OggVorbis SOURCE CODE IS (C) COPYRIGHT 1994-2002             *
 * by the XIPHOPHORUS Company http://www.xiph.org/                  *
 *                                                                  *
 ********************************************************************

 function: simple programmatic interface for encoder mode setup
 last mod: $Id: vorbisenc.c,v 1.39.2.5 2002/06/11 04:44:46 xiphmont Exp $

 ********************************************************************/

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "vorbis/codec.h"
#include "vorbis/vorbisenc.h"

#include "codec_internal.h"

#include "os.h"
#include "misc.h"

/* careful with this; it's using static array sizing to make managing
   all the modes a little less annoying.  If we use a residue backend
   with > 10 partition types, or a different division of iteration,
   this needs to be updated. */

typedef struct {
  vorbis_info_residue0 *res[2];
  static_codebook *book_aux[2];
  static_codebook *book_aux_managed[2];
  static_codebook *books_base[10][3];
} vorbis_residue_template;

typedef struct vp_adjblock{
  int block[P_BANDS][P_LEVELS];
} vp_adjblock;

typedef struct {
  int data[NOISE_COMPAND_LEVELS];
} compandblock;

/* high level configuration information for setting things up
   step-by-step with the detailed vorbis_encode_ctl interface.
   There's a fair amount of redundancy such that interactive setup
   does not directly deal with any vorbis_info or codec_setup_info
   initialization; it's all stored (until full init) in this highlevel
   setup, then flushed out to the real codec setup structs later. */

typedef struct { int data[P_NOISECURVES]; } adj3; 
typedef struct { int data[PACKETBLOBS]; } adjB; 
typedef struct {
  int lo;
  int hi;
  int fixed;
} noiseguard;
typedef struct {
  int data[P_NOISECURVES][17];
} noise3;
typedef struct {
  float data[27];
} athcurve;

typedef struct {
  int      mappings;
  double  *rate_mapping;
  double  *quality_mapping;
  int      coupling_restriction;
  long     bitrate_min_restriction;
  long     bitrate_max_restriction;


  int     *blocksize_short;
  int     *blocksize_long;

  adj3    *psy_tone_masteratt;
  int     *psy_tone_0dB;
  int     *psy_tone_masterdepth;        // attempt to eliminate
  int     *psy_tone_dBsuppress;

  vp_adjblock *psy_tone_adj_impulse;
  vp_adjblock *psy_tone_adj_long;
  vp_adjblock *psy_tone_adj_other;
  vp_adjblock *psy_tone_depth;        // attempt to eliminate

  noiseguard  *psy_noiseguards;
  noise3      *psy_noise_bias_impulse;
  noise3      *psy_noise_bias_long;
  noise3      *psy_noise_bias_other;
  int         *psy_noise_dBsuppress;

  compandblock  *psy_noise_compand;
  double        *psy_noise_compand_short_mapping;
  double        *psy_noise_compand_long_mapping;

  int      *psy_noise_normal_start[2];
  int      *psy_noise_normal_partition[2];

  int      *psy_ath_float;
  int      *psy_ath_abs;
  athcurve *psy_ath;
  double   *psy_ath_mapping;

  double   *psy_lowpass;

  vorbis_info_psy_global *global_params;
  double *global_mapping;
  adjB   *stereo_modes;
  adjB   *stereo_pkHz;


  static_codebook ***floor_books;
  vorbis_info_floor1 *floor_params;
  int *floor_short_mapping;
  int *floor_long_mapping;

  vorbis_residue_template *residue;
  int res_type;
} ve_setup_data_template;

#include "modes/setup_44.h"

static ve_setup_data_template *setup_list[]={
  &ve_setup_44_stereo,


  0
};


/* a few static coder conventions */
static vorbis_info_mode _mode_template[2]={
  {0,0,0,-1},
  {1,0,0,-1}
};

/* mapping conventions:
   only one submap (this would change for efficient 5.1 support for example)*/
/* Four psychoacoustic profiles are used, one for each blocktype */
static vorbis_info_mapping0 _mapping_template[2]={
  {1, {0,0}, {0}, {-1}, 0,{0},{0}},
  {1, {0,0}, {1}, {-1}, 0,{0},{0}}
};

static int vorbis_encode_toplevel_setup(vorbis_info *vi,int ch,long rate){
  if(vi && vi->codec_setup){

    vi->version=0;
    vi->channels=ch;
    vi->rate=rate;

    return(0);
  }
  return(OV_EINVAL);
}

static int vorbis_encode_floor_setup(vorbis_info *vi,double s,int block,
				     static_codebook    ***books, 
				     vorbis_info_floor1 *in, 
				     int *x){
  int i,k,is=rint(s);
  vorbis_info_floor1 *f=_ogg_calloc(1,sizeof(*f));
  codec_setup_info *ci=vi->codec_setup;

  memcpy(f,in+x[is],sizeof(*f));
  /* fill in the lowpass field, even if it's temporary */
  f->n=ci->blocksizes[block]>>1;

  /* books */
  {
    int partitions=f->partitions;
    int maxclass=-1;
    int maxbook=-1;
    for(i=0;i<partitions;i++)
      if(f->partitionclass[i]>maxclass)maxclass=f->partitionclass[i];
    for(i=0;i<=maxclass;i++){
      if(f->class_book[i]>maxbook)maxbook=f->class_book[i];
      f->class_book[i]+=ci->books;
      for(k=0;k<(1<<f->class_subs[i]);k++){
	if(f->class_subbook[i][k]>maxbook)maxbook=f->class_subbook[i][k];
	if(f->class_subbook[i][k]>=0)f->class_subbook[i][k]+=ci->books;
      }
    }

    for(i=0;i<=maxbook;i++)
      ci->book_param[ci->books++]=books[x[is]][i];
  }

  /* for now, we're only using floor 1 */
  ci->floor_type[ci->floors]=1;
  ci->floor_param[ci->floors]=f;
  ci->floors++;

  return(0);
}

static int vorbis_encode_global_psych_setup(vorbis_info *vi,double s,
					    vorbis_info_psy_global *in, 
					    double *x){
  int i,is=s;
  double ds=s-is;
  codec_setup_info *ci=vi->codec_setup;
  vorbis_info_psy_global *g=&ci->psy_g_param;
  
  memcpy(g,in+(int)x[is],sizeof(*g));
  
  ds=x[is]*(1.-ds)+x[is+1]*ds;
  is=(int)ds;
  ds-=is;
  if(ds==0 && is>0){
    is--;
    ds=1.;
  }
  
  /* interpolate the trigger threshholds */
  for(i=0;i<4;i++){
    g->preecho_thresh[i]=in[is].preecho_thresh[i]*(1.-ds)+in[is+1].preecho_thresh[i]*ds;
    g->postecho_thresh[i]=in[is].postecho_thresh[i]*(1.-ds)+in[is+1].postecho_thresh[i]*ds;
  }
  g->ampmax_att_per_sec=ci->hi.amplitude_track_dBpersec;
  return(0);
}

static int vorbis_encode_global_stereo(vorbis_info *vi,
				       highlevel_encode_setup *hi,
				       adjB *pdB,
				       adjB *pkHz){
  float s=hi->stereo_point_setting;
  int i,is=s;
  double ds=s-is;
  codec_setup_info *ci=vi->codec_setup;
  vorbis_info_psy_global *g=&ci->psy_g_param;

  if(hi->managed){
    memcpy(g->coupling_pointamp,pdB[is].data,sizeof(*pdB));
    /* interpolate the kHz threshholds */
    for(i=0;i<PACKETBLOBS;i++){
      float kHz=pkHz[is*2].data[i]*(1.-ds)+pkHz[is*2+2].data[i]*ds;
      g->coupling_pointlimit[0][i]=kHz*1000./vi->rate*ci->blocksizes[0];
      kHz=pkHz[is*2+1].data[i]*(1.-ds)+pkHz[is*2+3].data[i]*ds;
      g->coupling_pointlimit[1][i]=kHz*1000./vi->rate*ci->blocksizes[1];
    }
  }else{
    int point_dB=pdB[is].data[PACKETBLOBS/2];
    float kHz=pkHz[is*2].data[PACKETBLOBS/2]*(1.-ds)+pkHz[is*2+2].data[PACKETBLOBS/2]*ds;
    for(i=0;i<PACKETBLOBS;i++)
      g->coupling_pointamp[i]=point_dB;
    for(i=0;i<PACKETBLOBS;i++)
      g->coupling_pointlimit[0][i]=kHz*1000./vi->rate*ci->blocksizes[0];
    kHz=pkHz[is*2+1].data[PACKETBLOBS/2]*(1.-ds)+pkHz[is*2+3].data[PACKETBLOBS/2]*ds;
    for(i=0;i<PACKETBLOBS;i++)
      g->coupling_pointlimit[1][i]=kHz*1000./vi->rate*ci->blocksizes[1];
  }

  return(0);
}

static int vorbis_encode_psyset_setup(vorbis_info *vi,double s,
				      int *nn_start,
				      int *nn_partition,
				      int block){
  codec_setup_info *ci=vi->codec_setup;
  vorbis_info_psy *p=ci->psy_param[block];
  highlevel_encode_setup *hi=&ci->hi;
  int is=s;
  
  if(block>=ci->psys)
    ci->psys=block+1;
  if(!p){
    p=_ogg_calloc(1,sizeof(*p));
    ci->psy_param[block]=p;
  }
  
  memcpy(p,&_psy_info_template,sizeof(*p));
  p->blockflag=block>>1;

  if(hi->noise_normalize_p){
    p->normal_channel_p=1;
    p->normal_point_p=1;
    p->normal_start=nn_start[is];
    p->normal_partition=nn_partition[is];
  }
    
  return 0;
}

static int vorbis_encode_tonemask_setup(vorbis_info *vi,double s,int block,
					adj3 *att,
					int  *max,
					vp_adjblock *in){
  int i,j,is=s;
  double ds=s-is;
  codec_setup_info *ci=vi->codec_setup;
  vorbis_info_psy *p=ci->psy_param[block];

  /* 0 and 2 are only used by bitmanagement, but there's no harm to always
     filling the values in here */
  p->tone_masteratt[0]=att[is].data[0]*(1.-ds)+att[is+1].data[0]*ds;
  p->tone_masteratt[1]=att[is].data[1]*(1.-ds)+att[is+1].data[1]*ds;
  p->tone_masteratt[2]=att[is].data[2]*(1.-ds)+att[is+1].data[2]*ds;

  p->max_curve_dB=max[is]*(1.-ds)+max[is+1]*ds;

  for(i=0;i<P_BANDS;i++)
    for(j=0;j<P_LEVELS;j++)
      p->toneatt.block[i][j]=(j<4?4:j)*-10.+
	in[is].block[i][j]*(1.-ds)+in[is+1].block[i][j]*ds;
  return(0);
}


static int vorbis_encode_compand_setup(vorbis_info *vi,double s,int block,
				       compandblock *in, double *x){
  int i,is=s;
  double ds=s-is;
  codec_setup_info *ci=vi->codec_setup;
  vorbis_info_psy *p=ci->psy_param[block];

  ds=x[is]*(1.-ds)+x[is+1]*ds;
  is=(int)ds;
  ds-=is;
  if(ds==0 && is>0){
    is--;
    ds=1.;
  }

  /* interpolate the compander settings */
  for(i=0;i<NOISE_COMPAND_LEVELS;i++)
    p->noisecompand[i]=in[is].data[i]*(1.-ds)+in[is+1].data[i]*ds;
  return(0);
}

static int vorbis_encode_peak_setup(vorbis_info *vi,double s,int block,
				    int *guard,
				    int *suppress,
				    vp_adjblock *in){
  int i,j,is=s;
  double ds=s-is;
  codec_setup_info *ci=vi->codec_setup;
  vorbis_info_psy *p=ci->psy_param[block];

  p->peakattp=1;
  p->tone_guard=guard[is]*(1.-ds)+guard[is+1]*ds;
  p->tone_abs_limit=suppress[is]*(1.-ds)+suppress[is+1]*ds;

  for(i=0;i<P_BANDS;i++)
    for(j=0;j<P_LEVELS;j++)
      p->peakatt.block[i][j]=(j<4?4:j)*-10.+
	in[is].block[i][j]*(1.-ds)+in[is+1].block[i][j]*ds;
  return(0);
}

static int vorbis_encode_noisebias_setup(vorbis_info *vi,double s,int block,
					 int *suppress,
					 noise3 *in,
					 noiseguard *guard){
  int i,is=s,j;
  double ds=s-is;
  codec_setup_info *ci=vi->codec_setup;
  vorbis_info_psy *p=ci->psy_param[block];

  p->noisemaxsupp=suppress[is]*(1.-ds)+suppress[is+1]*ds;
  p->noisewindowlomin=guard[is].lo;
  p->noisewindowhimin=guard[is].hi;
  p->noisewindowfixed=guard[is].fixed;

  for(j=0;j<P_NOISECURVES;j++)
    for(i=0;i<P_BANDS;i++)
      p->noiseoff[j][i]=in[is].data[j][i]*(1.-ds)+in[is+1].data[j][i]*ds;

  return(0);
}

static int vorbis_encode_ath_setup(vorbis_info *vi,double s,int block,
				   athcurve *in, double *x){
  int i,is=s;
  double ds;
  codec_setup_info *ci=vi->codec_setup;
  vorbis_info_psy *p=ci->psy_param[block];

  p->ath_adjatt=ci->hi.ath_floating_dB;
  p->ath_maxatt=ci->hi.ath_absolute_dB;

  ds=x[is]*(1.-ds)+x[is+1]*ds;
  is=(int)ds;
  ds-=is;
  if(ds==0 && is>0){
    is--;
    ds=1.;
  }

  for(i=0;i<27;i++)
    p->ath[i]=in[is].data[i]*(1.-ds)+in[is+1].data[i]*ds;
  return(0);
}


static int book_dup_or_new(codec_setup_info *ci,static_codebook *book){
  int i;
  for(i=0;i<ci->books;i++)
    if(ci->book_param[i]==book)return(i);
  
  return(ci->books++);
}

static void vorbis_encode_blocksize_setup(vorbis_info *vi,double s,
					 int *shortb,int *longb){

  codec_setup_info *ci=vi->codec_setup;
  int is=s;
  
  int blockshort=shortb[is];
  int blocklong=longb[is];
  ci->blocksizes[0]=blockshort;
  ci->blocksizes[1]=blocklong;

}

static int vorbis_encode_residue_setup(vorbis_info *vi,double s,int block,
				       int type,
				       vorbis_residue_template *in){

  codec_setup_info *ci=vi->codec_setup;
  int i,is=s;
  int n,k;
  int number=block;

  vorbis_info_residue0 *r=ci->residue_param[number]=
    _ogg_malloc(sizeof(*r));
  vorbis_info_mapping0 *map=ci->map_param[number]=
    _ogg_calloc(1,sizeof(*map));
  vorbis_info_mode *mode=ci->mode_param[number]=
    _ogg_calloc(1,sizeof(*mode));
  
  memcpy(ci->mode_param[number],&_mode_template[block],
	 sizeof(*_mode_template));
  if(number>=ci->modes)ci->modes=number+1;
  ci->mode_param[number]->mapping=number;
  ci->mode_param[number]->blockflag=block;
  
  ci->map_type[number]=0;
  memcpy(ci->map_param[number],&_mapping_template[block],sizeof(*map));
  if(number>=ci->maps)ci->maps=number+1;
  ((vorbis_info_mapping0 *)(ci->map_param[number]))->residuesubmap[0]=number;

  memcpy(r,in[is].res[block],sizeof(*r));
  if(ci->residues<=number)ci->residues=number+1;

  switch(ci->blocksizes[block]){
  case 64:case 128:case 256:case 512:
    r->grouping=16;
    break;
  default:
    r->grouping=32;
    break;
  }
  ci->residue_type[number]=type;

  switch(ci->residue_type[number]){
  case 1:
    n=r->end=ci->blocksizes[block]>>1; /* to be adjusted by lowpass later */
    break;
  case 2:
    n=r->end=(ci->blocksizes[block]>>1)*vi->channels; /* to be adjusted by lowpass later */
    break;
  }
  
  for(i=0;i<r->partitions;i++)
    for(k=0;k<3;k++)
      if(in[is].books_base[i][k])
	r->secondstages[i]|=(1<<k);
  
  if(type==2){ /* that is to say, if we're coupling, which will always mean
		  res type 2 in this encoder for now */
    vorbis_info_mapping0 *map=ci->map_param[number];
    
    map->coupling_steps=1;
    map->coupling_mag[0]=0;
    map->coupling_ang[0]=1;
    
  }

  /* fill in all the books */
  {
    int booklist=0,k;
    r->groupbook=book_dup_or_new(ci,in[is].book_aux[block]);
    ci->book_param[r->groupbook]=in[is].book_aux[block];
    for(i=0;i<r->partitions;i++){
      for(k=0;k<3;k++){
	if(in[is].books_base[i][k]){
	  int bookid=book_dup_or_new(ci,in[is].books_base[i][k]);
	  r->booklist[booklist++]=bookid;
	  ci->book_param[bookid]=in[is].books_base[i][k];
	}
      }
    }
  }
  
  /* lowpass setup */
  {
    double freq=ci->hi.lowpass_kHz[block]*1000.;
    vorbis_info_floor1 *f=ci->floor_param[block];
    double nyq=vi->rate/2.;
    long blocksize=ci->blocksizes[block]>>1;
    
    if(freq>vi->rate/2)freq=vi->rate/2;
    /* lowpass needs to be set in the floor and the residue. */
    
    /* in the floor, the granularity can be very fine; it doesn't alter
       the encoding structure, only the samples used to fit the floor
       approximation */
    f->n=freq/nyq*blocksize; 
    
    /* in the residue, we're constrained, physically, by partition
       boundaries.  We still lowpass 'wherever', but we have to round up
       here to next boundary, or the vorbis spec will round it *down* to
       previous boundary in encode/decode */
    if(ci->residue_type[block]==2)
      r->end=(int)((freq/nyq*blocksize*2)/r->grouping+.9)* /* round up only if we're well past */
	r->grouping;
    else
      r->end=(int)((freq/nyq*blocksize)/r->grouping+.9)* /* round up only if we're well past */
	r->grouping;
  }
   
  return(0);
}      

/* encoders will need to use vorbis_info_init beforehand and call
   vorbis_info clear when all done */

/* two interfaces; this, more detailed one, and later a convenience
   layer on top */

/* the final setup call */
int vorbis_encode_setup_init(vorbis_info *vi){
  int ret=0,i0=0;
  codec_setup_info *ci=vi->codec_setup;
  ve_setup_data_template *setup=NULL;
  highlevel_encode_setup *hi=&ci->hi;

  if(ci==NULL)return(OV_EINVAL);
  if(!hi->impulse_block_p)i0=1;

  /* too low/high an ATH floater is nonsensical, but doesn't break anything */
  if(hi->ath_floating_dB>-80)hi->ath_floating_dB=-80;
  if(hi->ath_floating_dB<-200)hi->ath_floating_dB=-200;

  /* again, bound this to avoid the app shooting itself int he foot
     too badly */
  if(hi->amplitude_track_dBpersec>0.)hi->amplitude_track_dBpersec=0.;
  if(hi->amplitude_track_dBpersec<-99999.)hi->amplitude_track_dBpersec=-99999.;
  
  /* get the appropriate setup template; matches the fetch in previous
     stages */
  setup=(ve_setup_data_template *)hi->setup;
  if(setup==NULL)return(OV_EINVAL);

  /* choose block sizes from configured sizes as well as paying
     attention to long_block_p and short_block_p.  If the configured
     short and long blocks are the same length, we set long_block_p
     and unset short_block_p */
  vorbis_encode_blocksize_setup(vi,hi->base_setting,
				setup->blocksize_short,
				setup->blocksize_long);
  
  /* floor setup; choose proper floor params.  Allocated on the floor
     stack in order; if we alloc only long floor, it's 0 */
  ret|=vorbis_encode_floor_setup(vi,hi->short_setting,0,
				 setup->floor_books,
				 setup->floor_params,
				 setup->floor_short_mapping);
  ret|=vorbis_encode_floor_setup(vi,hi->long_setting,1,
				 setup->floor_books,
				 setup->floor_params,
				 setup->floor_long_mapping);
  
  /* setup of [mostly] short block detection */
  ret|=vorbis_encode_global_psych_setup(vi,hi->trigger_setting,
					setup->global_params,
					setup->global_mapping);
  
  ret|=vorbis_encode_global_stereo(vi,hi,
				   setup->stereo_modes,
				   setup->stereo_pkHz);

  /* basic psych setup and noise normalization */
  ret|=vorbis_encode_psyset_setup(vi,hi->short_setting,
				  setup->psy_noise_normal_start[0],
				  setup->psy_noise_normal_partition[0],  
				  0);
  ret|=vorbis_encode_psyset_setup(vi,hi->short_setting,
				  setup->psy_noise_normal_start[0],
				  setup->psy_noise_normal_partition[0],  
				  1);
  ret|=vorbis_encode_psyset_setup(vi,hi->long_setting,
				  setup->psy_noise_normal_start[1],
				  setup->psy_noise_normal_partition[1],  
				  2);
  ret|=vorbis_encode_psyset_setup(vi,hi->long_setting,
				  setup->psy_noise_normal_start[1],
				  setup->psy_noise_normal_partition[1],  
				  3);

  /* tone masking setup */
  ret|=vorbis_encode_tonemask_setup(vi,hi->block[i0].tone_mask_setting,0,
				    setup->psy_tone_masteratt,
				    setup->psy_tone_0dB,
				    setup->psy_tone_adj_impulse);
  ret|=vorbis_encode_tonemask_setup(vi,hi->block[1].tone_mask_setting,1,
				    setup->psy_tone_masteratt,
				    setup->psy_tone_0dB,
				    setup->psy_tone_adj_other);
  ret|=vorbis_encode_tonemask_setup(vi,hi->block[2].tone_mask_setting,2,
				    setup->psy_tone_masteratt,
				    setup->psy_tone_0dB,
				    setup->psy_tone_adj_other);
  ret|=vorbis_encode_tonemask_setup(vi,hi->block[3].tone_mask_setting,3,
				    setup->psy_tone_masteratt,
				    setup->psy_tone_0dB,
				    setup->psy_tone_adj_long);

  /* noise companding setup */
  ret|=vorbis_encode_compand_setup(vi,hi->block[i0].noise_compand_setting,0,
				   setup->psy_noise_compand,
				   setup->psy_noise_compand_short_mapping);
  ret|=vorbis_encode_compand_setup(vi,hi->block[1].noise_compand_setting,1,
				   setup->psy_noise_compand,
				   setup->psy_noise_compand_short_mapping);
  ret|=vorbis_encode_compand_setup(vi,hi->block[2].noise_compand_setting,2,
				   setup->psy_noise_compand,
				   setup->psy_noise_compand_long_mapping);
  ret|=vorbis_encode_compand_setup(vi,hi->block[3].noise_compand_setting,3,
				   setup->psy_noise_compand,
				   setup->psy_noise_compand_long_mapping);

  /* peak guarding setup */
  ret|=vorbis_encode_peak_setup(vi,hi->block[i0].tone_peaklimit_setting,0,
				setup->psy_tone_masterdepth,
				setup->psy_tone_dBsuppress,
				setup->psy_tone_depth);
  ret|=vorbis_encode_peak_setup(vi,hi->block[1].tone_peaklimit_setting,1,
				setup->psy_tone_masterdepth,
				setup->psy_tone_dBsuppress,
				setup->psy_tone_depth);
  ret|=vorbis_encode_peak_setup(vi,hi->block[2].tone_peaklimit_setting,2,
				setup->psy_tone_masterdepth,
				setup->psy_tone_dBsuppress,
				setup->psy_tone_depth);
  ret|=vorbis_encode_peak_setup(vi,hi->block[3].tone_peaklimit_setting,3,
				setup->psy_tone_masterdepth,
				setup->psy_tone_dBsuppress,
				setup->psy_tone_depth);

  /* noise bias setup */
  ret|=vorbis_encode_noisebias_setup(vi,hi->block[i0].noise_bias_setting,0,
				     setup->psy_noise_dBsuppress,
				     setup->psy_noise_bias_impulse,
				     setup->psy_noiseguards);
  ret|=vorbis_encode_noisebias_setup(vi,hi->block[1].noise_bias_setting,1,
				     setup->psy_noise_dBsuppress,
				     setup->psy_noise_bias_other,
				     setup->psy_noiseguards);
  ret|=vorbis_encode_noisebias_setup(vi,hi->block[2].noise_bias_setting,2,
				     setup->psy_noise_dBsuppress,
				     setup->psy_noise_bias_other,
				     setup->psy_noiseguards);
  ret|=vorbis_encode_noisebias_setup(vi,hi->block[3].noise_bias_setting,3,
				     setup->psy_noise_dBsuppress,
				     setup->psy_noise_bias_long,
				     setup->psy_noiseguards);

  ret|=vorbis_encode_ath_setup(vi,hi->ath_setting,0,
			       setup->psy_ath,
			       setup->psy_ath_mapping);
  ret|=vorbis_encode_ath_setup(vi,hi->ath_setting,1,
			       setup->psy_ath,
			       setup->psy_ath_mapping);
  ret|=vorbis_encode_ath_setup(vi,hi->ath_setting,2,
			       setup->psy_ath,
			       setup->psy_ath_mapping);
  ret|=vorbis_encode_ath_setup(vi,hi->ath_setting,3,
			       setup->psy_ath,
				 setup->psy_ath_mapping);

  if(ret){
    vorbis_info_clear(vi);
    return ret; 
  }

  ret|=vorbis_encode_residue_setup(vi,hi->short_setting,0,
				   setup->res_type,
				   setup->residue);  
  ret|=vorbis_encode_residue_setup(vi,hi->long_setting,1,
				   setup->res_type,
				   setup->residue);
    
  if(ret)
    vorbis_info_clear(vi);

  /* set bitrate readonlies and management */
  vi->bitrate_nominal=setting_to_approx_bitrate(vi);
  vi->bitrate_lower=hi->bitrate_min;
  vi->bitrate_upper=hi->bitrate_max;
  vi->bitrate_window=hi->bitrate_limit_window;

  if(hi->managed){
    ci->bi.queue_avg_time=hi->bitrate_av_window;
    ci->bi.queue_avg_center=hi->bitrate_av_window_center;
    ci->bi.queue_minmax_time=hi->bitrate_limit_window;
    ci->bi.queue_hardmin=hi->bitrate_min;
    ci->bi.queue_hardmax=hi->bitrate_max;
    ci->bi.queue_avgmin=hi->bitrate_av_lo;
    ci->bi.queue_avgmax=hi->bitrate_av_hi;
    ci->bi.avgfloat_downslew_max=999999.f;
    ci->bi.avgfloat_upslew_max=999999.f;
  }

  return(ret);
  
}

static double setting_to_approx_bitrate(vorbis_info *vi){
  codec_setup_info *ci=vi->codec_setup;
  highlevel_encode_setup *hi=&ci->hi;
  ve_setup_data_template *setup=(ve_setup_data_template *)hi->setup;
  int is=hi->base_setting;
  double ds=hi->base_setting-is;
  int ch=vi->channels;
  double *r=setup->rate_mapping;

  if(r==NULL)
    return(-1);
  
  return((r[is]*(1.-ds)+r[is+1]*ds)*ch);  
}

static void get_setup_template(vorbis_info *vi,
			       long ch,long srate,
			       double req,int q_or_bitrate){
  int i=0,j;
  codec_setup_info *ci=vi->codec_setup;
  highlevel_encode_setup *hi=&ci->hi;

  while(setup_list[i]){
    if(setup_list[i]->coupling_restriction==-1 ||
       setup_list[i]->coupling_restriction==ch){
      if(srate>=setup_list[i]->bitrate_min_restriction &&
	 srate<=setup_list[i]->bitrate_max_restriction){
	int mappings=setup_list[i]->mappings;
	double *map=(q_or_bitrate?
		     setup_list[i]->rate_mapping:
		     setup_list[i]->quality_mapping);
	if(q_or_bitrate)req/=ch;

	/* the template matches.  Does the requested quality mode
	   fall within this template's modes? */
	if(req<map[0])continue;
	if(req>map[setup_list[i]->mappings])continue;
	for(j=0;j<mappings;j++)
	  if(req>=map[j] && req<map[j+1])break;
	/* an all-points match */
	hi->setup=setup_list[i];
	if(j==mappings)
	  hi->base_setting=j-.001;
	else{
	  float low=map[j];
	  float high=map[j+1];
	  float del=(req-low)/(high-low);
	  hi->base_setting=j+del;
	}
	return;
      }
    }
  }
  
  hi->setup=NULL;
}

static int vorbis_encode_setup_setting(vorbis_info *vi,
				       long  channels,
				       long  rate){
  int ret=0,i,is;
  codec_setup_info *ci=vi->codec_setup;
  highlevel_encode_setup *hi=&ci->hi;
  ve_setup_data_template *setup=hi->setup;
  double ds;

  ret=vorbis_encode_toplevel_setup(vi,channels,rate);
  if(ret)return(ret);

  is=hi->base_setting;
  ds=hi->base_setting-is;

  hi->short_setting=hi->base_setting;
  hi->long_setting=hi->base_setting;

  hi->managed=0;

  hi->impulse_block_p=1;
  //hi->noise_normalize_p=1;

  hi->stereo_point_setting=hi->base_setting;
  hi->lowpass_kHz[0]=
    hi->lowpass_kHz[1]=
    setup->psy_lowpass[is]*(1.-ds)+setup->psy_lowpass[is+1]*ds;  
  
  hi->ath_floating_dB=setup->psy_ath_float[is]*(1.-ds)+
    setup->psy_ath_float[is+1]*ds;
  hi->ath_absolute_dB=setup->psy_ath_abs[is]*(1.-ds)+
    setup->psy_ath_abs[is+1]*ds;

  hi->amplitude_track_dBpersec=-6.;
  hi->trigger_setting=hi->base_setting;
  hi->ath_setting=hi->base_setting;

  for(i=0;i<4;i++){
    hi->block[i].tone_mask_setting=hi->base_setting;
    hi->block[i].tone_peaklimit_setting=hi->base_setting;
    hi->block[i].noise_bias_setting=hi->base_setting;
    hi->block[i].noise_compand_setting=hi->base_setting;
  }

  return(ret);
}

int vorbis_encode_setup_vbr(vorbis_info *vi,
			    long  channels,
			    long  rate,			    
			    float quality){
  codec_setup_info *ci=vi->codec_setup;
  highlevel_encode_setup *hi=&ci->hi;

  get_setup_template(vi,channels,rate,quality,0);
  if(!hi->setup)return OV_EIMPL;
  
  return vorbis_encode_setup_setting(vi,channels,rate);
}

int vorbis_encode_init_vbr(vorbis_info *vi,
			   long channels,
			   long rate,
			   
			   float base_quality /* 0. to 1. */
			   ){
  int ret=0;

  ret=vorbis_encode_setup_vbr(vi,channels,rate,base_quality);
  
  if(ret){
    vorbis_info_clear(vi);
    return ret; 
  }
  ret=vorbis_encode_setup_init(vi);
  if(ret)
    vorbis_info_clear(vi);
  return(ret);
}

int vorbis_encode_setup_managed(vorbis_info *vi,
				long channels,
				long rate,
				
				long max_bitrate,
				long nominal_bitrate,
				long min_bitrate){

  codec_setup_info *ci=vi->codec_setup;
  highlevel_encode_setup *hi=&ci->hi;
  double tnominal=nominal_bitrate;
  int ret=0;

  if(nominal_bitrate<=0.){
    if(max_bitrate>0.){
      nominal_bitrate=max_bitrate*.875;
    }else{
      if(min_bitrate>0.){
	nominal_bitrate=min_bitrate;
      }else{
	return(OV_EINVAL);
      }
    }
  }

  get_setup_template(vi,channels,rate,nominal_bitrate,1);
  if(!hi->setup)return OV_EIMPL;
  
  ret=vorbis_encode_setup_setting(vi,channels,rate);
  if(ret){
    vorbis_info_clear(vi);
    return ret; 
  }

  /* initialize management with sane defaults */
  ci->hi.managed=1;

  ci->hi.bitrate_av_window=4.;
  ci->hi.bitrate_av_window_center=.5;
  ci->hi.bitrate_limit_window=2.;
  ci->hi.bitrate_min=min_bitrate;
  ci->hi.bitrate_max=max_bitrate;
  ci->hi.bitrate_av_lo=tnominal;
  ci->hi.bitrate_av_hi=tnominal;

  return(ret);
}

int vorbis_encode_init(vorbis_info *vi,
		       long channels,
		       long rate,

		       long max_bitrate,
		       long nominal_bitrate,
		       long min_bitrate){

  int ret=vorbis_encode_setup_managed(vi,channels,rate,
				      max_bitrate,
				      nominal_bitrate,
				      min_bitrate);
  if(ret){
    vorbis_info_clear(vi);
    return(ret);
  }

  ret=vorbis_encode_setup_init(vi);
  if(ret)
    vorbis_info_clear(vi);
  return(ret);
}

int vorbis_encode_ctl(vorbis_info *vi,int number,void *arg){
  return(OV_EIMPL);
}
