/**
 * @author Yunpeng Men
 * @email ypmen@pku.edu.cn
 * @create date 2020-11-04 20:47:29
 * @modify date 2020-11-04 20:47:29
 * @desc [description]
 */

#define FAST 1
#define NSBLK 1024

#include <iostream>
#include <iomanip>
#include <string.h>
#include <boost/program_options.hpp>

#include "pulsarsearch.h"
#include "subdedispersion.h"
#include "dedisperse.h"
#include "filterbank.h"
#include "utils.h"
#include "mjd.h"

using namespace std;
using namespace boost::program_options;

unsigned int num_threads;
unsigned int dbscan_radius;
unsigned int dbscan_k;

bool dumptim=true;

int main(int argc, const char *argv[])
{
    /* options */
	int verbose = 0;

	options_description desc{"Options"};
	desc.add_options()
			("help,h", "Help")
			("verbose,v", "Print debug information")
			("threads,t", value<unsigned int>()->default_value(1), "Number of threads")
			("jump,j", value<vector<double>>()->multitoken()->default_value(vector<double>{7200, 0}, "7200, 0"), "Time jump at the beginning and end (s)")
			("td", value<int>()->default_value(1), "Time downsample")
			("fd", value<int>()->default_value(1), "Frequency downsample")
			("dms", value<double>()->default_value(0), "DM start")
			("ddm", value<double>()->default_value(1), "DM step")
			("ndm", value<int>()->default_value(200), "Number of DM")
			("ddplan", value<string>(), "Input ddplan file")
			("seglen,l", value<float>()->default_value(1), "Time length per segment (s)")
			("ibeam,i", value<int>()->default_value(1), "Beam number")
			("rfi,z", value<vector<string>>()->multitoken()->zero_tokens()->composing(), "RFI mitigation [[mask tdRFI fdRFI] [kadaneF tdRFI fdRFI] [kadaneT tdRFI fdRFI] [zap fl fh] [zdot] [zero]]")
			("bandlimit", value<double>()->default_value(10), "Band limit of RFI mask (MHz)")
			("bandlimitKT", value<double>()->default_value(10), "Band limit of RFI kadaneT (MHz)")
			("widthlimit", value<double>()->default_value(10e-3), "Width limit of RFI kadaneF (s)")
			("threKadaneF", value<float>()->default_value(7), "S/N threshold of KadaneF")
			("threKadaneT", value<float>()->default_value(7), "S/N threshold of KadaneT")
			("threMask", value<float>()->default_value(3), "S/N threshold of Mask")
            ("rootname,o", value<string>()->default_value("J0000-00"), "Output rootname")
			("cont", "Input files are contiguous")
			("input,f", value<vector<string>>()->multitoken()->composing(), "Input files");

    positional_options_description pos_desc;
    pos_desc.add("input", -1);
	command_line_parser parser{argc, argv};
	parser.options(desc).style(command_line_style::default_style | command_line_style::allow_short);
	parser.options(desc).positional(pos_desc);
	parsed_options parsed_options = parser.run();

	variables_map vm;
	store(parsed_options, vm);
	notify(vm);

	if (vm.count("help"))
	{
		std::cout << desc << '\n';
		return 0;
	}
	if (vm.count("verbose"))
	{
		verbose = 1;
	}
	if (vm.count("input") == 0)
	{
		cerr<<"Error: no input file"<<endl;
		return -1;
	}

	bool contiguous = vm.count("cont");

    string rootname = vm["rootname"].as<string>();

    num_threads = vm["threads"].as<unsigned int>();

	vector<double> jump = vm["jump"].as<vector<double>>();

	vector<string> fnames = vm["input"].as<vector<string>>();

	long int nfil = fnames.size();
	Filterbank *fil = new Filterbank [nfil];
	for (long int i=0; i<nfil; i++)
	{
		fil[i].filename = fnames[i];
	}

	vector<MJD> tstarts;
	vector<MJD> tends;
	long int ntotal = 0;
	for (long int i=0; i<nfil; i++)
	{
        fil[i].read_header();
        ntotal += fil[i].nsamples;
        MJD tstart(fil[i].tstart);
        tstarts.push_back(tstart);
        tends.push_back(tstart+fil[i].nsamples*fil[i].tsamp);
	}
	vector<size_t> idx = argsort(tstarts);
	for (long int i=0; i<nfil-1; i++)
	{
		if (abs((tends[idx[i]]-tstarts[idx[i+1]]).to_second())>0.5*fil[idx[i]].tsamp)
		{
			if (contiguous)
			{
				cerr<<"Warning: time not contiguous"<<endl;
			}
			else
			{
				cerr<<"Error: time not contiguous"<<endl;
				exit(-1);
			}
		}
	}

	int ibeam = vm["ibeam"].as<int>();

	if (vm["ibeam"].defaulted())
	{
		if (fil[0].ibeam != 0)
			ibeam = fil[0].ibeam;
	}

	long int nchans = fil[0].nchans;
    double tsamp = fil[0].tsamp;
    int nifs = fil[0].nifs;

	float *buffer = new float [nchans];

	vector<PulsarSearch> search;
	plan(vm, search);

	vector<int> tds;
	for (auto sp=search.begin(); sp!=search.end(); ++sp)
	{
		tds.push_back((*sp).td);
	}

	long int td_lcm = findlcm(&tds[0], tds.size());

	long int ndump = (int)(vm["seglen"].as<float>()/tsamp)/td_lcm*td_lcm;

	DataBuffer<float> databuf(ndump, nchans);
	databuf.tsamp = tsamp;
	memcpy(&databuf.frequencies[0], fil[0].frequency_table, sizeof(double)*nchans);

	long int nseg = jump[0]/tsamp;
	long int njmp = jump[1]/tsamp;

    long int ncover = 0;

    stringstream ss_ibeam;
    ss_ibeam << "M" << setw(2) << setfill('0') << ibeam;
    string s_ibeam = ss_ibeam.str();

    ncover++;
	long int nsearch = search.size();
	for (long int k=0; k<nsearch; k++)
	{
		search[k].ibeam = ibeam;
        search[k].rootname = rootname + "_" + s_ibeam + '_' + to_string(ncover);
		search[k].prepare(databuf);
	}

	int sumif = nifs>2? 2:nifs;
	
    long int jmpcont = 0;
	long int ntot = 0;
	long int ntot2 = 0;
    long int count = 0;
    long int bcnt1 = 0;
	for (long int idxn=0; idxn<nfil; idxn++)
	{
		long int n = idx[idxn];
		long int nsegments = ceil(1.*fil[0].nsamples/NSBLK);
        long int ns_filn = 0;

		for (long int s=0; s<nsegments; s++)
		{
			if (verbose)
			{
				cerr<<"\r\rfinish "<<setprecision(2)<<fixed<<tsamp*count<<" seconds ";
				cerr<<"("<<100.*count/ntotal<<"%)";
			}

			fil[n].read_data(NSBLK);
#ifdef FAST
			unsigned char *pcur = (unsigned char *)(fil[n].data);
#endif
			for (long int i=0; i<NSBLK; i++)
			{
                count++;
                if (++ns_filn == fil[n].nsamples)
                {
                    goto next;
                }

                if (ntot == nseg)
                {
                    if (jmpcont++ < njmp)
                    {
                        pcur += nifs*nchans;
                        continue;
                    }
                   
                    ntot = 0;
                    jmpcont = 0;

                    ncover++;
                    for (long int k=0; k<nsearch; k++)
	                {
                        search[k].dedisp.rootname = rootname + "_" + s_ibeam + '_' + to_string(ncover);
                        search[k].dedisp.prepare(search[k].rfi);
                        search[k].dedisp.preparedump();
	                }
                }

				memset(buffer, 0, sizeof(float)*nchans);
				long int m = 0;
				for (long int k=0; k<sumif; k++)
				{
					for (long int j=0; j<nchans; j++)
					{
						buffer[j] +=  pcur[m++];
					}
				}

				memcpy(&databuf.buffer[0]+bcnt1*nchans, buffer, sizeof(float)*1*nchans);
                bcnt1++;
                ntot++;

				if (ntot%ndump == 0)
				{
					for (auto sp=search.begin(); sp!=search.end(); ++sp)
					{
						(*sp).run(databuf);
					}
                    bcnt1 = 0;
				}

				pcur += nifs*nchans;
			}
		}
        next:
		fil[n].close();
	}

	if (verbose)
	{
		cerr<<"\r\rfinish "<<setprecision(2)<<fixed<<tsamp*count<<" seconds ";
		cerr<<"("<<100.*count/ntotal<<"%)"<<endl;
	}

	delete [] buffer;
	delete [] fil;

    return 0;
}
