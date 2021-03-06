/*
  ZynAddSubFX - a software synthesizer
 
  SUBnote.C - The "subtractive" synthesizer
  Copyright (C) 2002-2005 Nasca Octavian Paul
  Author: Nasca Octavian Paul

  This program is free software; you can redistribute it and/or modify
  it under the terms of version 2 of the GNU General Public License 
  as published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License (version 2) for more details.

  You should have received a copy of the GNU General Public License (version 2)
  along with this program; if not, write to the Free Software Foundation,
  Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA

*/

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include "../globals.h"
#include "SUBnote.h"
#include "../Misc/Util.h"

SUBnote::SUBnote(SUBnoteParameters *parameters,Controller *ctl_,REALTYPE freq,REALTYPE velocity,int portamento_,int midinote){
    ready=0;
    
    tmpsmp=new REALTYPE[SOUND_BUFFER_SIZE];
    tmprnd=new REALTYPE[SOUND_BUFFER_SIZE];
    
    pars=parameters;
    ctl=ctl_;
    portamento=portamento_;
    NoteEnabled=ON;
    volume=pow(0.1,3.0*(1.0-pars->PVolume/96.0));//-60 dB .. 0 dB
    volume*=VelF(velocity,pars->PAmpVelocityScaleFunction);
    if (pars->PPanning!=0) panning=pars->PPanning/127.0;
	else panning=RND;
    numstages=pars->Pnumstages;
    stereo=pars->Pstereo;
    start=pars->Pstart;
    firsttick=1;
    int pos[MAX_SUB_HARMONICS];

    if (pars->Pfixedfreq==0) basefreq=freq;
	else {
	    basefreq=440.0;
	    int fixedfreqET=pars->PfixedfreqET;
	    if (fixedfreqET!=0) {//if the frequency varies according the keyboard note 
		REALTYPE tmp=(midinote-69.0)/12.0*(pow(2.0,(fixedfreqET-1)/63.0)-1.0);
		if (fixedfreqET<=64) basefreq*=pow(2.0,tmp);
		    else basefreq*=pow(3.0,tmp);
	    };

	};
    REALTYPE detune=getdetune(pars->PDetuneType,pars->PCoarseDetune,pars->PDetune);
    basefreq*=pow(2.0,detune/1200.0);//detune
//    basefreq*=ctl->pitchwheel.relfreq;//pitch wheel
    
    //global filter    
    GlobalFilterCenterPitch=pars->GlobalFilter->getfreq()+//center freq 
                        (pars->PGlobalFilterVelocityScale/127.0*6.0)* //velocity sensing
			(VelF(velocity,pars->PGlobalFilterVelocityScaleFunction)-1);

    GlobalFilterL=NULL;GlobalFilterR=NULL;
    GlobalFilterEnvelope=NULL;

    //select only harmonics that desire to compute    
    numharmonics=0;
    for (int n=0;n<MAX_SUB_HARMONICS;n++){
	if (pars->Phmag[n]==0)continue;
	if (n*basefreq>SAMPLE_RATE/2.0) break;//remove the freqs above the Nyquist freq
	pos[numharmonics++]=n;
    };
    
    if (numharmonics==0) {
	NoteEnabled=OFF;
	return;
    };
        
    
    lfilter=new bpfilter[numstages*numharmonics];
    if (stereo!=0) rfilter=new bpfilter[numstages*numharmonics];
    
    //how much the amplitude is normalised (because the harmonics)
    REALTYPE reduceamp=0.0;
    
    for (int n=0;n<numharmonics;n++){

	REALTYPE freq=basefreq*(pos[n]+1);
	
	//the bandwidth is not absolute(Hz); it is relative to frequency
	REALTYPE bw=pow(10,(pars->Pbandwidth-127.0)/127.0*4)*numstages;

	//Bandwidth Scale
	bw*=pow(1000/freq,(pars->Pbwscale-64.0)/64.0*3.0);

	//Relative BandWidth    
	bw*=pow(100,(pars->Phrelbw[pos[n]]-64.0)/64.0);

	if (bw>25.0) bw=25.0;

	//try to keep same amplitude on all freqs and bw. (empirically)
	REALTYPE gain=sqrt(1500.0/(bw*freq));

        REALTYPE hmagnew=1.0-pars->Phmag[pos[n]]/127.0;
	REALTYPE hgain;

        switch(pars->Phmagtype){
	    case 1:hgain=exp(hmagnew*log(0.01)); break;
	    case 2:hgain=exp(hmagnew*log(0.001));break;
	    case 3:hgain=exp(hmagnew*log(0.0001));break;
	    case 4:hgain=exp(hmagnew*log(0.00001));break;
	    default:hgain=1.0-hmagnew;
	};
	gain*=hgain;
        reduceamp+=hgain;

        for (int nph=0;nph<numstages;nph++){
	    REALTYPE amp=1.0;
    	    if (nph==0) amp=gain;
	    initfilter(lfilter[nph+n*numstages],freq,bw,amp,hgain);
	    if (stereo!=0) initfilter(rfilter[nph+n*numstages],freq,bw,amp,hgain);
	};
    };
    
    if (reduceamp<0.001) reduceamp=1.0;
    volume/=reduceamp;
    
    oldpitchwheel=0;
    oldbandwidth=64;
    if (pars->Pfixedfreq==0) initparameters(basefreq);
	else initparameters(basefreq/440.0*freq);

    oldamplitude=newamplitude;
    ready=1;
};

SUBnote::~SUBnote(){
    if (NoteEnabled!=OFF) KillNote();
    delete [] tmpsmp;
    delete [] tmprnd;
};

/*
 * Kill the note
 */
void SUBnote::KillNote(){
    if (NoteEnabled!=OFF){
	delete [] lfilter;
	lfilter=NULL;
	if (stereo!=0) delete [] rfilter;
	rfilter=NULL;
	delete(AmpEnvelope);
	if (FreqEnvelope!=NULL) delete(FreqEnvelope);
	if (BandWidthEnvelope!=NULL) delete(BandWidthEnvelope);
	NoteEnabled=OFF;
    };
    
};


/*
 * Compute the filters coefficients
 */
void SUBnote::computefiltercoefs(bpfilter &filter,REALTYPE freq,REALTYPE bw,REALTYPE gain){
    if (freq>SAMPLE_RATE/2.0-200.0) {
	freq=SAMPLE_RATE/2.0-200.0;
    };

    REALTYPE omega=2.0*PI*freq/SAMPLE_RATE;
    REALTYPE sn=sin(omega);REALTYPE cs=cos(omega);
    REALTYPE alpha=sn*sinh(LOG_2/2.0*bw*omega/sn);

    if (alpha>1) alpha=1;
    if (alpha>bw) alpha=bw;
    
    filter.b0=alpha/(1.0+alpha)*filter.amp*gain;
    filter.b2=-alpha/(1.0+alpha)*filter.amp*gain;
    filter.a1=-2.0*cs/(1.0+alpha); 
    filter.a2=(1.0-alpha)/(1.0+alpha);

};


/*
 * Initialise the filters
 */
void SUBnote::initfilter(bpfilter &filter,REALTYPE freq,REALTYPE bw,REALTYPE amp,REALTYPE mag){
    filter.xn1=0.0;filter.xn2=0.0;
    
    if (start==0) {
	filter.yn1=0.0;
	filter.yn2=0.0;
    } else {
	REALTYPE a=0.1*mag;//empirically
        REALTYPE p=RND*2.0*PI;
	if (start==1) a*=RND;
	filter.yn1=a*cos(p);
	filter.yn2=a*cos(p+freq*2.0*PI/SAMPLE_RATE);

	//correct the error of computation the start amplitude 
	//at very high frequencies	
	if (freq>SAMPLE_RATE*0.96) {
	    filter.yn1=0.0;
	    filter.yn2=0.0;
	
	};
    };

    filter.amp=amp;   
    filter.freq=freq;
    filter.bw=bw;
    computefiltercoefs(filter,freq,bw,1.0);
};

/*
 * Do the filtering
 */
void SUBnote::filter(bpfilter &filter,REALTYPE *smps){
    int i;
    REALTYPE out;
    for (i=0;i<SOUND_BUFFER_SIZE;i++){
       out=smps[i] * filter.b0 + filter.b2 * filter.xn2
          -filter.a1 * filter.yn1 - filter.a2 * filter.yn2;
       filter.xn2=filter.xn1;
       filter.xn1=smps[i];
       filter.yn2=filter.yn1;
       filter.yn1=out;
       smps[i]=out;

    };
};

/*
 * Init Parameters
 */
void SUBnote::initparameters(REALTYPE freq){
    AmpEnvelope=new Envelope(pars->AmpEnvelope,freq);
    if (pars->PFreqEnvelopeEnabled!=0) FreqEnvelope=new Envelope(pars->FreqEnvelope,freq);
	    else FreqEnvelope=NULL;
    if (pars->PBandWidthEnvelopeEnabled!=0) BandWidthEnvelope=new Envelope(pars->BandWidthEnvelope,freq);
	    else BandWidthEnvelope=NULL;
    if (pars->PGlobalFilterEnabled!=0){
	globalfiltercenterq=pars->GlobalFilter->getq();
	GlobalFilterL=new Filter(pars->GlobalFilter);
	if (stereo!=0) GlobalFilterR=new Filter(pars->GlobalFilter);
	GlobalFilterEnvelope=new Envelope(pars->GlobalFilterEnvelope,freq);
	GlobalFilterFreqTracking=pars->GlobalFilter->getfreqtracking(basefreq);
    };
    computecurrentparameters();
};


/*
 * Compute Parameters of SUBnote for each tick
 */
void SUBnote::computecurrentparameters(){
    if ((FreqEnvelope!=NULL)||(BandWidthEnvelope!=NULL)||
	(oldpitchwheel!=ctl->pitchwheel.data)||
	(oldbandwidth!=ctl->bandwidth.data)||
	(portamento!=0)){
	REALTYPE envfreq=1.0;
	REALTYPE envbw=1.0;
	REALTYPE gain=1.0;
		
	if (FreqEnvelope!=NULL) {
	    envfreq=FreqEnvelope->envout()/1200;
	    envfreq=pow(2.0,envfreq);
	};
	envfreq*=ctl->pitchwheel.relfreq;//pitch wheel
	if (portamento!=0) {//portamento is used
	    envfreq*=ctl->portamento.freqrap;
	    if (ctl->portamento.used==0){//the portamento has finished
		portamento=0;//this note is no longer "portamented"
	    };
	};
	
	if (BandWidthEnvelope!=NULL) {
	    envbw=BandWidthEnvelope->envout();
	    envbw=pow(2,envbw);	    
	};
	envbw*=ctl->bandwidth.relbw;//bandwidth controller

	REALTYPE tmpgain=1.0/sqrt(envbw*envfreq);

	for (int n=0;n<numharmonics;n++){
	    for (int nph=0;nph<numstages;nph++) {
		if (nph==0) gain=tmpgain;else gain=1.0;
	        computefiltercoefs( lfilter[nph+n*numstages],
	    			    lfilter[nph+n*numstages].freq*envfreq,
				    lfilter[nph+n*numstages].bw*envbw,gain);
	    };
	};
	if (stereo!=0)
	for (int n=0;n<numharmonics;n++){
	    for (int nph=0;nph<numstages;nph++) {
		if (nph==0) gain=tmpgain;else gain=1.0;
	        computefiltercoefs( rfilter[nph+n*numstages],
	    			    rfilter[nph+n*numstages].freq*envfreq,
				    rfilter[nph+n*numstages].bw*envbw,gain);
	    };
	};
	oldbandwidth=ctl->bandwidth.data;
	oldpitchwheel=ctl->pitchwheel.data;
    };
    newamplitude=volume*AmpEnvelope->envout_dB()*2.0;
    
    //Filter
    if (GlobalFilterL!=NULL){
	REALTYPE globalfilterpitch=GlobalFilterCenterPitch+GlobalFilterEnvelope->envout();
	REALTYPE filterfreq=globalfilterpitch+ctl->filtercutoff.relfreq+GlobalFilterFreqTracking;
	filterfreq=GlobalFilterL->getrealfreq(filterfreq);
	
	GlobalFilterL->setfreq_and_q(filterfreq,globalfiltercenterq*ctl->filterq.relq);
	if (GlobalFilterR!=NULL) GlobalFilterR->setfreq_and_q(filterfreq,globalfiltercenterq*ctl->filterq.relq);
    };

};

/*
 * Note Output
 */
int SUBnote::noteout(REALTYPE *outl,REALTYPE *outr){
    int i;

    for (i=0;i<SOUND_BUFFER_SIZE;i++){
	outl[i]=denormalkillbuf[i];
	outr[i]=denormalkillbuf[i];
    };
    
    if (NoteEnabled==OFF) return(0);

    //left channel
    for (i=0;i<SOUND_BUFFER_SIZE;i++) tmprnd[i]=RND*2.0-1.0;
    for (int n=0;n<numharmonics;n++){
	for (i=0;i<SOUND_BUFFER_SIZE;i++) tmpsmp[i]=tmprnd[i];
	for (int nph=0;nph<numstages;nph++) 
	     filter(lfilter[nph+n*numstages],tmpsmp);     
	for (i=0;i<SOUND_BUFFER_SIZE;i++) outl[i]+=tmpsmp[i];
    };

    if (GlobalFilterL!=NULL) GlobalFilterL->filterout(&outl[0]);     
    
    //right channel
    if (stereo!=0){
	for (i=0;i<SOUND_BUFFER_SIZE;i++) tmprnd[i]=RND*2.0-1.0;
	for (int n=0;n<numharmonics;n++){
	    for (i=0;i<SOUND_BUFFER_SIZE;i++) tmpsmp[i]=tmprnd[i];
	    for (int nph=0;nph<numstages;nph++) 
	        filter(rfilter[nph+n*numstages],tmpsmp);     
	    for (i=0;i<SOUND_BUFFER_SIZE;i++) outr[i]+=tmpsmp[i];
	};
	if (GlobalFilterR!=NULL) GlobalFilterR->filterout(&outr[0]);     
    } else for (i=0;i<SOUND_BUFFER_SIZE;i++) outr[i]=outl[i];
    
    if (firsttick!=0){
	int n=10;if (n>SOUND_BUFFER_SIZE) n=SOUND_BUFFER_SIZE;
	for (i=0;i<n;i++) {
	    REALTYPE ampfadein=0.5-0.5*cos((REALTYPE) i/(REALTYPE) n*PI);
	    outl[i]*=ampfadein;
	    outr[i]*=ampfadein;
	};
	firsttick=0;
    };

    if (ABOVE_AMPLITUDE_THRESHOLD(oldamplitude,newamplitude)){
	// Amplitude interpolation 
        for (i=0;i<SOUND_BUFFER_SIZE;i++){
	   REALTYPE tmpvol=INTERPOLATE_AMPLITUDE(oldamplitude
	          ,newamplitude,i,SOUND_BUFFER_SIZE);
	   outl[i]*=tmpvol*panning; 
	   outr[i]*=tmpvol*(1.0-panning);
        };
    } else {
	for (i=0;i<SOUND_BUFFER_SIZE;i++) {
	    outl[i]*=newamplitude*panning;
	    outr[i]*=newamplitude*(1.0-panning);
	};
    };

    oldamplitude=newamplitude;    
    computecurrentparameters();
    
    // Check if the note needs to be computed more
    if (AmpEnvelope->finished()!=0){
        for (i=0;i<SOUND_BUFFER_SIZE;i++) {//fade-out
		REALTYPE tmp=1.0-(REALTYPE)i/(REALTYPE)SOUND_BUFFER_SIZE;
		outl[i]*=tmp;
		outr[i]*=tmp;
		};
	KillNote();    
    };
    return(1);
};

/*
 * Relase Key (Note Off)
 */
void SUBnote::relasekey(){
    AmpEnvelope->relasekey();
    if (FreqEnvelope!=NULL) FreqEnvelope->relasekey();
    if (BandWidthEnvelope!=NULL) BandWidthEnvelope->relasekey();
    if (GlobalFilterEnvelope!=NULL) GlobalFilterEnvelope->relasekey();
};

/*
 * Check if the note is finished
 */
int SUBnote::finished(){
  if (NoteEnabled==OFF) return(1);
	else return(0);    
};

