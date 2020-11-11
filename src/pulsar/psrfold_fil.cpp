/**
 * @author Yunpeng Men
 * @email ypmen@pku.edu.cn
 * @create date 2020-11-04 14:14:27
 * @modify date 2020-11-04 14:14:27
 * @desc [description]
 */

#define FAST 1
#define NSBLK 1024

#include "config.h"

#include <fstream>
#include <iostream>
#include <iomanip>
#include <string.h>
#include <utility>
#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp> 

#include "dedisperse.h"
#include "archivewriter.h"

#ifdef HAVE_PYTHON
	#include "pulsarplot.h"
#endif

#include "gridsearch.h"
#include "archivelite.h"
#include "dedispersionlite.h"
#include "databuffer.h"
#include "downsample.h"
#include "rfi.h"
#include "equalize.h"
#include "filterbank.h"
#include "mjd.h"
#include "utils.h"

using namespace std;
using namespace boost::program_options;

unsigned int num_threads;
bool dumptim=false;

void produce(variables_map &vm, Pulsar::DedispersionLite &dedisp, vector<Pulsar::ArchiveLite> &folder);

int main(int argc, const char *argv[])
{
    /* options */
	int verbose = 0;

	options_description desc{"Options"};
	desc.add_options()
			("help,h", "Help")
			("verbose,v", "Print debug information")
			("threads,t", value<unsigned int>()->default_value(1), "Number of threads")
			("jump,j", value<vector<double>>()->multitoken()->default_value(vector<double>{0, 0}, "0, 0"), "Time jump at the beginning and end (s)")
			("td", value<int>()->default_value(1), "Time downsample")
			("fd", value<int>()->default_value(1), "Frequency downsample")
			("dm", value<double>()->default_value(0), "DM (pc/cc)")
            ("f0", value<double>()->default_value(0), "F0 (Hz)")
            ("f1", value<double>()->default_value(0), "F1 (Hz/s)")
			("nosearch", "Do not search dm,f0,f1")
			("candfile", value<string>(), "Input cand file")
			("template", value<string>(), "Input fold template file")
			("nbin,b", value<int>()->default_value(64), "Number of bins per period")
			("tsubint,L", value<double>()->default_value(1), "Time length per integration (s)")
			("nsubband,n", value<int>()->default_value(32), "Number of subband")
			("srcname", value<string>()->default_value("PSRJ0000+00"), "Souce name")
			("telescope", value<string>()->default_value("Fake"), "Telescope name")
			("ibeam,i", value<int>()->default_value(1), "Beam number")
			("ra", value<double>()->default_value(0), "RA (hhmmss.s)")
			("dec", value<double>()->default_value(0), "DEC (ddmmss.s)")
			("rfi,z", value<vector<string>>()->multitoken()->zero_tokens()->composing(), "RFI mitigation [[mask tdRFI fdRFI] [kadaneF tdRFI fdRFI] [kadaneT tdRFI fdRFI] [zap fl fh] [zdot] [zero]]")
			("bandlimit", value<double>()->default_value(10), "Band limit of RFI mask (MHz)")
			("bandlimitKT", value<double>()->default_value(10), "Band limit of RFI kadaneT (MHz)")
			("widthlimit", value<double>()->default_value(10e-3), "Width limit of RFI kadaneF (s)")
			("tdRFI", value<int>()->default_value(1), "Time downsample of RFI")
			("fdRFI", value<int>()->default_value(1), "Frequency downsample of RFI")
			("threKadaneF", value<float>()->default_value(7), "S/N threshold of KadaneF")
			("threKadaneT", value<float>()->default_value(7), "S/N threshold of KadaneT")
			("threMask", value<float>()->default_value(10), "S/N threshold of Mask")
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

	bool nosearch = vm.count("nosearch");
	bool contiguous = vm.count("cont");
    string rootname = vm["rootname"].as<string>();
	string src_name = vm["srcname"].as<string>();
	string s_telescope = vm["telescope"].as<string>();
    num_threads = vm["threads"].as<unsigned int>();
    vector<double> jump = vm["jump"].as<vector<double>>();
    vector<string> fnames = vm["input"].as<vector<string>>();
	double src_raj = vm["ra"].as<double>();
	double src_dej = vm["dec"].as<double>();

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

	if (vm["srcname"].defaulted())
	{
		if (strcmp(fil[0].source_name, "") != 0)
			src_name = fil[0].source_name;
	}
	if (vm["telescope"].defaulted())
	{
		get_telescope_name(fil[0].telescope_id, s_telescope);
	}

	if (vm["ibeam"].defaulted())
	{
		if (fil[0].ibeam != 0)
			ibeam = fil[0].ibeam;
	}

	if (vm["ra"].defaulted())
	{
		if (fil[0].src_raj != 0.)
		{
			src_raj = fil[0].src_raj;
		}
	}
	if (vm["dec"].defaulted())
	{
		if (fil[0].src_dej != 0.)
		{
			src_dej = fil[0].src_dej;
		}
	}

	/** downsample */
	int td = vm["td"].as<int>();
    int fd = vm["fd"].as<int>();

    /** rfi */
	vector<pair<double, double>> zaplist;
	vector<vector<string>> rfilist;
    vector<string> rfi_opts;
	if (vm.count("rfi"))
	{
        rfi_opts = vm["rfi"].as<vector<string>>();
        for (auto opt=rfi_opts.begin(); opt!=rfi_opts.end(); ++opt)
        {
            if (*opt=="mask" or *opt=="kadaneF" or *opt=="kadaneT")
            {
                vector<string> temp{*opt, *(opt+1), *(opt+2)};       
                rfilist.push_back(temp);
                advance(opt, 2);
            }
            else if (*opt == "zap")
            {
                zaplist.push_back(pair<double, double>(stod(*(opt+1)), stod(*(opt+2))));
                advance(opt, 2);
            }
            else if (*opt=="zero" or *opt=="zdot")
            {
                vector<string> temp{*opt};
                rfilist.push_back(temp);
            }
        }
	}

	double bandlimit = vm["bandlimit"].as<double>();
    double bandlimitKT = vm["bandlimitKT"].as<double>();
    double widthlimit = vm["widthlimit"].as<double>();
    int tdRFI = vm["tdRFI"].as<int>();
	int fdRFI = vm["fdRFI"].as<int>();
    float threKadaneF = vm["threKadaneF"].as<float>();
    float threKadaneT = vm["threKadaneT"].as<float>();
    float threMask = vm["threMask"].as<float>();

	long int nchans = fil[0].nchans;
    double tsamp = fil[0].tsamp;
    int nifs = fil[0].nifs;

	float *buffer = new float [nchans];

    long int ndump = (int)(vm["tsubint"].as<double>()/tsamp)/td*td;

	DataBuffer<float> databuf(ndump, nchans);
	databuf.tsamp = tsamp;
	memcpy(&databuf.frequencies[0], fil[0].frequency_table, sizeof(double)*nchans);

	long int nstart = jump[0]/tsamp;
	long int nend = ntotal-jump[1]/tsamp;

	Downsample downsample;
	downsample.td = td;
    downsample.fd = fd;
    downsample.prepare(databuf);

	Equalize equalize;
	equalize.prepare(downsample);

    RFI rfi;
	rfi.prepare(equalize);
    
    Pulsar::DedispersionLite dedisp;
	vector<Pulsar::ArchiveLite> folder;
	produce(vm, dedisp, folder);
    dedisp.prepare(rfi);

    DataBuffer<float> subdata;
    dedisp.get_subdata(subdata, 0);

	long int ncand = folder.size();

    for (long int k=0; k<ncand; k++)
	{
        folder[k].start_mjd = tstarts[idx[0]];
		folder[k].ref_epoch = tstarts[idx[0]]+(ntotal*tsamp/2.);
        folder[k].resize(1, subdata.nchans, folder[k].nbin);
		folder[k].prepare(subdata);
        folder[k].dm = dedisp.vdm[k];
	}

    int sumif = nifs>2? 2:nifs;
	
	long int ntot = 0;
	long int ntot2 = 0;
	long int count = 0;
    long int bcnt1 = 0;
	for (long int idxn=0; idxn<nfil; idxn++)
	{
		long int n = idx[idxn];
        long int nseg = ceil(1.*fil[0].nsamples/NSBLK);
        long int ns_filn = 0;

		for (long int s=0; s<nseg; s++)
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

				if (count-1<nstart or count-1>nend)
				{
					pcur += nifs*nchans;
					continue;
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
    				downsample.run(databuf);

    				equalize.run(downsample);

					rfi.zap(equalize, zaplist);

					for (auto irfi = rfilist.begin(); irfi!=rfilist.end(); ++irfi)
                    {
                        if ((*irfi)[0] == "mask")
                        {
                            rfi.mask(rfi, threMask, stoi((*irfi)[1]), stoi((*irfi)[2]));
                        }
                        else if ((*irfi)[0] == "kadaneF")
                        {
                            rfi.kadaneF(rfi, threKadaneF*threKadaneF, widthlimit, stoi((*irfi)[1]), stoi((*irfi)[2]));
                        }
                        else if ((*irfi)[0] == "kadaneT")
                        {
                            rfi.kadaneT(rfi, threKadaneT*threKadaneT, bandlimitKT, stoi((*irfi)[1]), stoi((*irfi)[2]));
                        }
                        else if ((*irfi)[0] == "zdot")
                        {
                            rfi.zdot(rfi);
                        }
                        else if ((*irfi)[0] == "zero")
                        {
                            rfi.zero(rfi);
                        }
                    }

                    dedisp.run(rfi);

					for (long int k=0; k<ncand; k++)
					{
                        dedisp.get_subdata(subdata, k);
                        if (dedisp.counter > dedisp.offset+dedisp.ndump)
						{
							folder[k].run(subdata);
						}
					}

                    bcnt1 = 0;
				}

				pcur += nifs*nchans;
			}
		}
        next:
		fil[n].close();
	}

	double fmin = 1e6;
    double fmax = 0.;
    for (long int j=0; j<databuf.nchans; j++)
    {
        fmax = databuf.frequencies[j]>fmax? databuf.frequencies[j]:fmax;
        fmin = databuf.frequencies[j]<fmin? databuf.frequencies[j]:fmin;
    }

	double tint = ntotal*tsamp;

	vector<Pulsar::GridSearch> gridsearch(ncand);
#ifdef _OPENMP
#pragma omp parallel for num_threads(num_threads)
#endif
	for (long int k=0; k<ncand; k++)
	{
		double dm = folder[k].dm;
		double f0 = folder[k].f0;
		double f1 = folder[k].f1;

		gridsearch[k].ddmstart = -3*1./f0/Pulsar::DedispersionLite::dmdelay(1, fmax, fmin);
		gridsearch[k].ddmstep = abs(gridsearch[k].ddmstart/folder[k].nbin);
		gridsearch[k].nddm = 2*folder[k].nbin;
		gridsearch[k].df0start = -3*1./tint;
		gridsearch[k].df0step = abs(gridsearch[k].df0start/folder[k].nbin);
		gridsearch[k].ndf0 = 2*folder[k].nbin;
		gridsearch[k].df1start = -3*2./(tint*tint);
		gridsearch[k].df1step = abs(gridsearch[k].df1start/folder[k].nbin);
		gridsearch[k].ndf1 = 2*folder[k].nbin;

		gridsearch[k].prepare(folder[k]);
		
		if (!nosearch)
		{
			double dm0 = gridsearch[k].dm;
			double f00 = gridsearch[k].f0;
			double f10 = gridsearch[k].f1;
			double dm1 = gridsearch[k].dm + 2*gridsearch[k].ddmstep;
			double f01 = gridsearch[k].f0 + 2*gridsearch[k].df0step;
			double f11 = gridsearch[k].f1 + 2*gridsearch[k].df1step;
			int cont = 0;
			while ((abs(dm0-dm1)>gridsearch[k].ddmstep or abs(f00-f01)>gridsearch[k].df0step or abs(f10-f11)>gridsearch[k].df1step) and cont<8)
			{
				dm0 = gridsearch[k].dm;
				f00 = gridsearch[k].f0;
				f10 = gridsearch[k].f1;

				gridsearch[k].runFFdot();
				gridsearch[k].bestprofiles();
				gridsearch[k].runDM();
				gridsearch[k].bestprofiles();

				dm1 = gridsearch[k].dm;
				f01 = gridsearch[k].f0;
				f11 = gridsearch[k].f1;

				cont++;
				//cout<<gridsearch[k].dm<<" "<<gridsearch[k].f0<<" "<<gridsearch[k].f1<<endl;
			}
		}
	}

	/** form obsinfo*/
	std::map<std::string, std::string> obsinfo;
	//source name
	obsinfo["Source_name"] = src_name;
	//start mjd
	stringstream ss_mjd;
    ss_mjd << setprecision(10) << fixed << tstarts[idx[0]].to_day();
    string s_mjd = ss_mjd.str();
	obsinfo["Date"] = s_mjd;
	//ra dec string
	string s_ra, s_dec;
	get_s_radec(src_raj, src_dej, s_ra, s_dec);
	obsinfo["RA"] = s_ra;
	obsinfo["DEC"] = s_dec;
	//telescope
	obsinfo["Telescope"] = s_telescope;
	//beam
	stringstream ss_ibeam;
    ss_ibeam << "M" << setw(4) << setfill('0') << ibeam;
    string s_ibeam = ss_ibeam.str();
	obsinfo["Beam"] = s_ibeam;	
	//data filename
	obsinfo["Filename"] = fnames[idx[0]];
	//observation length
	obsinfo["Obslen"] = to_string(tint);

	ArchiveWriter writer;
    writer.template_file = vm["template"].as<string>();
    writer.mode = Integration::FOLD;
    writer.ibeam = 1;
    writer.src_name = src_name;
	writer.ra = s_ra;
	writer.dec = s_dec;

	for (long int k=0; k<ncand; k++)
	{
		stringstream ss_id;
		ss_id << setw(4) << setfill('0') << k+1;
		string s_id = ss_id.str();

		writer.rootname = rootname + "_" + obsinfo["Date"] + "_" + s_ibeam + "_" + s_id;

		writer.prepare(folder[k], gridsearch[k]);
		writer.run(folder[k], gridsearch[k]);
		writer.close();

#ifdef HAVE_PYTHON
		Pulsar::PulsarPlot psrplot;
		psrplot.plot(dedisp, folder[k], gridsearch[k], obsinfo, k+1, rootname);
#endif
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

void produce(variables_map &vm, Pulsar::DedispersionLite &dedisp, vector<Pulsar::ArchiveLite> &folder)
{
    Pulsar::ArchiveLite fdr;

    /** archive */
    fdr.f0 = vm["f0"].as<double>();
    fdr.f1 = vm["f1"].as<double>();
    fdr.nbin = vm["nbin"].as<int>();

    dedisp.vdm.push_back(vm["dm"].as<double>());
    dedisp.nsubband = vm["nsubband"].as<int>();

    if (vm.count("candfile"))
    {
        string filename = vm["candfile"].as<string>();
        string line;
        ifstream candfile(filename);
        
        dedisp.vdm.clear();
        while (getline(candfile, line))
        {
			if (line.rfind("#", 0) == 0) continue;
            vector<string> parameters;
            boost::split(parameters, line, boost::is_any_of("\t "), boost::token_compress_on);

            dedisp.vdm.push_back(stod(parameters[1]));
            fdr.f0 = stod(parameters[3]);
            fdr.f1 = stod(parameters[4]);

            folder.push_back(fdr);
        }
    }
    else
    {
        folder.push_back(fdr);
    }
}