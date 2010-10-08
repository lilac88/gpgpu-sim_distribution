/* 
 * Copyright (c) 2009 by Tor M. Aamodt, Wilson W. L. Fung, Ali Bakhoda, 
 * George L. Yuan, Dan O'Connor, Joey Ting, Henry Wong and the 
 * University of British Columbia
 * Vancouver, BC  V6T 1Z4
 * All Rights Reserved.
 * 
 * THIS IS A LEGAL DOCUMENT BY DOWNLOADING GPGPU-SIM, YOU ARE AGREEING TO THESE
 * TERMS AND CONDITIONS.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNERS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 * 
 * NOTE: The files libcuda/cuda_runtime_api.c and src/cuda-sim/cuda-math.h
 * are derived from the CUDA Toolset available from http://www.nvidia.com/cuda
 * (property of NVIDIA).  The files benchmarks/BlackScholes/ and 
 * benchmarks/template/ are derived from the CUDA SDK available from 
 * http://www.nvidia.com/cuda (also property of NVIDIA).  The files from 
 * src/intersim/ are derived from Booksim (a simulator provided with the 
 * textbook "Principles and Practices of Interconnection Networks" available 
 * from http://cva.stanford.edu/books/ppin/). As such, those files are bound by 
 * the corresponding legal terms and conditions set forth separately (original 
 * copyright notices are left in files from these sources and where we have 
 * modified a file our copyright notice appears before the original copyright 
 * notice).  
 * 
 * Using this version of GPGPU-Sim requires a complete installation of CUDA 
 * which is distributed seperately by NVIDIA under separate terms and 
 * conditions.  To use this version of GPGPU-Sim with OpenCL requires a
 * recent version of NVIDIA's drivers which support OpenCL.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 
 * 3. Neither the name of the University of British Columbia nor the names of
 * its contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 * 
 * 4. This version of GPGPU-SIM is distributed freely for non-commercial use only.  
 *  
 * 5. No nonprofit user may place any restrictions on the use of this software,
 * including as modified by the user, by any other authorized user.
 * 
 * 6. GPGPU-SIM was developed primarily by Tor M. Aamodt, Wilson W. L. Fung, 
 * Ali Bakhoda, George L. Yuan, at the University of British Columbia, 
 * Vancouver, BC V6T 1Z4
 */

#include "ptx_loader.h"
#include "ptx_ir.h"
#include "cuda-sim.h"
#include "ptx_parser.h"
#include <dirent.h>
#include <fstream>

/// globals

memory_space *g_global_mem;
memory_space *g_tex_mem;
memory_space *g_surf_mem;
memory_space *g_param_mem;
bool g_override_embedded_ptx = false;

/// extern prototypes

extern "C" int ptx_parse();
extern "C" int ptx__scan_string(const char*);

extern "C" const char *g_ptxinfo_filename = NULL;
extern "C" int ptxinfo_parse();
extern "C" int ptxinfo_debug;
extern "C" FILE *ptxinfo_in;

static bool g_save_embedded_ptx;
bool g_keep_intermediate_files;

void ptx_reg_options(option_parser_t opp)
{
   option_parser_register(opp, "-save_embedded_ptx", OPT_BOOL, &g_save_embedded_ptx, 
                "saves ptx files embedded in binary as <n>.ptx",
                "0");
   option_parser_register(opp, "-keep", OPT_BOOL, &g_keep_intermediate_files, 
                "keep intermediate files created by GPGPU-Sim when interfacing with external programs",
                "0");
}

void print_ptx_file( const char *p, unsigned source_num, const char *filename )
{
   printf("\nGPGPU-Sim PTX: file _%u.ptx contents:\n\n", source_num );
   char *s = strdup(p);
   char *t = s;
   unsigned n=1;
   while ( *t != '\0'  ) {
      char *u = t;
      while ( (*u != '\n') && (*u != '\0') ) u++;
      unsigned last = (*u == '\0');
      *u = '\0';
      const ptx_instruction *pI = ptx_instruction_lookup(filename,n);
      char pc[64];
      if( pI && pI->get_PC() )
         snprintf(pc,64,"%4u", pI->get_PC() );
      else 
         snprintf(pc,64,"    ");
      printf("    _%u.ptx  %4u (pc=%s):  %s\n", source_num, n, pc, t );
      if ( last ) break;
      t = u+1;
      n++;
   }
   free(s);
   fflush(stdout);
}

char* gpgpu_ptx_sim_convert_ptx_to_ptxplus(const char *ptx_str, const char *cubin_str, unsigned source_num, bool save_converted )
{
	printf("GPGPU-Sim PTX: converting EMBEDDED .ptx file to ptxplus \n");

	// Extract ptx to a file
    char fname_ptx[1024];
    snprintf(fname_ptx,1024,"_ptx_XXXXXX");
    int fd=mkstemp(fname_ptx);
    close(fd);

    printf("GPGPU-Sim PTX: extracting embedded .ptx to temporary file \"%s\"\n", fname_ptx);
    FILE *ptxfile = fopen(fname_ptx,"w");
    fprintf(ptxfile,"%s",ptx_str);
    fclose(ptxfile);

    // Extract cubin to a file
    char fname_cubin[1024];
    snprintf(fname_cubin,1024,"_cubin_XXXXXX");
    int fd2=mkstemp(fname_cubin);
    close(fd2);

    printf("GPGPU-Sim PTX: extracting embedded cubin to temporary file \"%s\"\n", fname_cubin);
    FILE *cubinfile = fopen(fname_cubin,"w");
    fprintf(cubinfile,"%s",cubin_str);
    fclose(cubinfile);

    // Run decuda
    char fname_decuda[1024];
    snprintf(fname_decuda,1024,"_decuda_XXXXXX");
    int fd3=mkstemp(fname_decuda);
    close(fd3);

    char decuda_commandline[1024];
    snprintf(decuda_commandline,1024,"$DECUDA_INSTALL_PATH/decuda.py -o %s %s", fname_decuda, fname_cubin);

    printf("GPGPU-Sim PTX: calling decuda on cubin file, decuda output file = \"%s\"\n", fname_decuda);
    int decuda_result = system(decuda_commandline);
	if( decuda_result != 0 ) {
	   printf("GPGPU-Sim PTX: ERROR ** while calling decuda (b) %d\n", decuda_result);
	   printf("               Ensure env variable DECUDA_INSTALL_PATH is set and points to decuda base directory.\n");
	   exit(1);
	}

	// Run decuda_to_ptxplus
    char fname_ptxplus[1024];
    snprintf(fname_ptxplus,1024,"_ptxplus_XXXXXX");
    int fd4=mkstemp(fname_ptxplus);
    close(fd4);

    char d2pp_commandline[1024];
    snprintf(d2pp_commandline,1024,"$D2PP_INSTALL_PATH/decuda_to_ptxplus %s %s %s %s > /dev/null", fname_decuda, fname_ptx, fname_cubin, fname_ptxplus);

    printf("GPGPU-Sim PTX: calling decuda_to_ptxplus, ptxplus output file = \"%s\"\n", fname_ptxplus);
    int d2pp_result = system(d2pp_commandline);
	if( d2pp_result != 0 ) {
	   printf("GPGPU-Sim PTX: ERROR ** while calling decuda_to_ptxplus %d\n", d2pp_result);
	   printf("               Ensure env variable D2PP_INSTALL_PATH is set and points to decuda_to_ptxplus base directory.\n");
	   exit(1);
	}

	// Get ptxplus from file
	std::ifstream fileStream(fname_ptxplus, std::ios::in);
	std::string text, line;
	while(getline(fileStream,line)) {
		text += (line + "\n");
	}
	fileStream.close();

	char* ptxplus_str = new char [strlen(text.c_str())+1];
	strcpy(ptxplus_str, text.c_str());

	// Save ptxplus to file if specified
	if(save_converted) {
	    char fname_ptxplus_save[1024];
	    snprintf(fname_ptxplus_save,1024,"_%u.ptxplus", source_num );
		printf("GPGPU-Sim PTX: saving converted ptxplus to file \"%s\"\n", fname_ptxplus_save);

	    FILE *file_ptxplus_save = fopen(fname_ptxplus_save,"w");
	    fprintf(file_ptxplus_save,"%s",ptxplus_str);
	    fclose(file_ptxplus_save);
	}

	// Remove temporary files
	char rm_commandline[1024];
	snprintf(rm_commandline,1024,"rm -f %s %s %s %s", fname_ptx, fname_cubin, fname_decuda, fname_ptxplus);
	printf("GPGPU-Sim PTX: removing temporary files using \"%s\"\n", rm_commandline);
	int rm_result = system(rm_commandline);
	if( rm_result != 0 ) {
	   printf("GPGPU-Sim PTX: ERROR ** while removing temporary files %d\n", rm_result);
	   exit(1);
	}

	printf("GPGPU-Sim PTX: DONE converting EMBEDDED .ptx file to ptxplus \n");

	return ptxplus_str;
}


symbol_table *gpgpu_ptx_sim_load_ptx_from_string( const char *p, unsigned source_num )
{
    char buf[1024];
    snprintf(buf,1024,"_%u.ptx", source_num );
    if( g_save_embedded_ptx ) {
       FILE *fp = fopen(buf,"w");
       fprintf(fp,"%s",p);
       fclose(fp);
    }
    symbol_table *symtab=init_parser(buf);
    ptx__scan_string(p);
    int errors = ptx_parse ();
    if ( errors ) {
        char fname[1024];
        snprintf(fname,1024,"_ptx_errors_XXXXXX");
        int fd=mkstemp(fname); 
        close(fd);
        printf("GPGPU-Sim PTX: parser error detected, exiting... but first extracting .ptx to \"%s\"\n", fname);
        FILE *ptxfile = fopen(fname,"w");
        fprintf(ptxfile,"%s", p );
        fclose(ptxfile);
        abort();
        exit(40);
    }

    if ( g_debug_execution >= 100 ) 
       print_ptx_file(p,source_num,buf);

    printf("GPGPU-Sim PTX: finished parsing EMBEDDED .ptx file %s\n",buf);
    return symtab;
}

void gpgpu_ptxinfo_load_from_string( const char *p_for_info, unsigned source_num )
{
    char fname[1024];
    snprintf(fname,1024,"_ptx_XXXXXX");
    int fd=mkstemp(fname); 
    close(fd);

    printf("GPGPU-Sim PTX: extracting embedded .ptx to temporary file \"%s\"\n", fname);
    FILE *ptxfile = fopen(fname,"w");
    fprintf(ptxfile,"%s", p_for_info);
    fclose(ptxfile);

    char fname2[1024];
    snprintf(fname2,1024,"_ptx2_XXXXXX");
    fd=mkstemp(fname2); 
    close(fd);
    char commandline2[4096];
    snprintf(commandline2,4096,"cat %s | sed 's/.version 1.5/.version 1.4/' | sed 's/, texmode_independent//' | sed 's/\\(\\.extern \\.const\\[1\\] .b8 \\w\\+\\)\\[\\]/\\1\\[1\\]/' | sed 's/const\\[.\\]/const\\[0\\]/g' > %s", fname, fname2);
    int result = system(commandline2);
    if( result != 0 ) {
       printf("GPGPU-Sim PTX: ERROR ** while loading PTX (a) %d\n", result);
       printf("               Ensure you have write access to simulation directory\n");
       printf("               and have \'cat\' and \'sed\' in your path.\n");
       exit(1);
    }

    char tempfile_ptxinfo[1024];
    snprintf(tempfile_ptxinfo,1024,"%sinfo",fname);
    char commandline[1024];
    char extra_flags[1024];
    extra_flags[0]=0;

#if CUDART_VERSION >= 3000
    snprintf(extra_flags,1024,"--gpu-name=sm_20");
#endif

    snprintf(commandline,1024,"ptxas %s -v %s --output-file /dev/null 2> %s", 
             extra_flags, fname2, tempfile_ptxinfo);
    printf("GPGPU-Sim PTX: generating ptxinfo using \"%s\"\n", commandline);
    result = system(commandline);
    if( result != 0 ) {
       printf("GPGPU-Sim PTX: ERROR ** while loading PTX (b) %d\n", result);
       printf("               Ensure ptxas is in your path.\n");
       exit(1);
    }

    ptxinfo_in = fopen(tempfile_ptxinfo,"r");
    g_ptxinfo_filename = tempfile_ptxinfo;
    ptxinfo_parse();
    snprintf(commandline,1024,"rm -f %s %s %s", fname, fname2, tempfile_ptxinfo);
    printf("GPGPU-Sim PTX: removing ptxinfo using \"%s\"\n", commandline);
    result = system(commandline);
    if( result != 0 ) {
       printf("GPGPU-Sim PTX: ERROR ** while loading PTX (c) %d\n", result);
       exit(1);
    }
}

