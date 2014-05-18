#include <iostream>
#include <unordered_map>
#include <map>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>

#include "pin.H"

const int MAXTHREADS = 64;

KNOB<int> COMMSIZE(KNOB_MODE_WRITEONCE, "pintool", "cs", "6", "comm shift in bits");
KNOB<int> PAGESIZE(KNOB_MODE_WRITEONCE, "pintool", "ps", "12", "page size in bits");
KNOB<int> INTERVAL(KNOB_MODE_WRITEONCE, "pintool", "i", "0", "print interval (ms) (0=disable)");

KNOB<bool> DOCOMM(KNOB_MODE_WRITEONCE, "pintool", "c", "0", "enable comm detection");
KNOB<bool> DOPAGE(KNOB_MODE_WRITEONCE, "pintool", "p", "0", "enable page usage detection");


int num_threads = 0;

UINT64 matrix[MAXTHREADS][MAXTHREADS];

unordered_map<UINT64, array<UINT64, MAXTHREADS+1>> pagemap;

unordered_map<UINT64, array<UINT32,2>> commmap;

map<UINT32, UINT32> pidmap;

void print_matrix();
void print_numa();


VOID mythread(VOID * arg)
{
	while(!PIN_IsProcessExiting()) {
		if (INTERVAL == 0) {
			PIN_Sleep(100);
			continue;
		} else {
			PIN_Sleep(INTERVAL);
		}

		if (DOCOMM) {
			print_matrix();
			memset(matrix, 0, sizeof(matrix));
		}
		if (DOPAGE) {
			print_numa();
			for(auto it : pagemap)
				fill(begin(it.second), end(it.second), 0);
		}
	}
}

static inline
VOID inc_comm(int a, int b) {
	if (a!=b-1)
		matrix[a][b-1]++;
}

VOID do_comm(ADDRINT addr, THREADID tid)
{
	UINT64 line = addr >> COMMSIZE;
	tid = tid>=2 ? tid-1 : tid;
	int sh = 1;

	THREADID a = commmap[line][0];
	THREADID b = commmap[line][1];


	if (a == 0 && b == 0)
		sh = 0;
	if (a != 0 && b != 0)
		sh = 2;

	switch (sh) {
		case 0: /* no one accessed line before, store accessing thread in pos 0 */
			commmap[line][0] = tid+1;
			break;

		case 1: /* one previous access => needs to be in pos 0 */
			if (a != tid+1) {
				inc_comm(tid, a);
				commmap[line][1] = a;
				commmap[line][0] = tid+1;
			}
			break;

		case 2: // two previous accesses
			if (a != tid+1 && b != tid+1) {
				inc_comm(tid, a);
				inc_comm(tid, b);
				commmap[line][1] = a;
				commmap[line][0] = tid+1;
			} else if (a == tid+1) {
				inc_comm(tid, b);
			} else if (b == tid+1) {
				inc_comm(tid, a);
				commmap[line][1] = a;
				commmap[line][0] = tid+1;
			}

			break;
	}
}

VOID do_numa(ADDRINT addr, THREADID tid)
{
	UINT64 page = addr >> PAGESIZE;
	tid = tid>=2 ? tid-1 : tid;

	if (pagemap[page][MAXTHREADS] == 0)
		__sync_bool_compare_and_swap(&pagemap[page][MAXTHREADS], 0, tid+1);

	pagemap[page][tid]++;
}


VOID trace_memory_comm(INS ins, VOID *v)
{
	if (INS_IsMemoryRead(ins))
		INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)do_comm, IARG_MEMORYREAD_EA, IARG_THREAD_ID, IARG_END);

	if (INS_HasMemoryRead2(ins))
		INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)do_comm, IARG_MEMORYREAD2_EA, IARG_THREAD_ID, IARG_END);

	if (INS_IsMemoryWrite(ins))
		INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)do_comm, IARG_MEMORYWRITE_EA, IARG_THREAD_ID, IARG_END);
}

VOID trace_memory_page(INS ins, VOID *v)
{
	if (INS_IsMemoryRead(ins))
		INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)do_numa, IARG_MEMORYREAD_EA, IARG_THREAD_ID, IARG_END);

	if (INS_HasMemoryRead2(ins))
		INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)do_numa, IARG_MEMORYREAD2_EA, IARG_THREAD_ID, IARG_END);

	if (INS_IsMemoryWrite(ins))
		INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)do_numa, IARG_MEMORYWRITE_EA, IARG_THREAD_ID, IARG_END);
}


VOID ThreadStart(THREADID threadid, CONTEXT *ctxt, INT32 flags, VOID *v)
{
	__sync_add_and_fetch(&num_threads, 1);
	int pid = PIN_GetTid();
	pidmap[pid] = threadid ? threadid - 1 : threadid;
}

VOID print_matrix()
{
	static long n = 0;
	ofstream f;
	char fname[255];

	sprintf(fname, "%06ld.comm.csv", n++);

	int real_tid[MAXTHREADS+1];
	int i = 0, a, b;

	for (auto it : pidmap)
		real_tid[it.second] = i++;

	cout << fname << endl;

	f.open(fname);

	for (int i = num_threads-1; i>=0; i--) {
		a = real_tid[i];
		for (int j = 0; j<num_threads; j++) {
			b = real_tid[j];
			f << matrix[a][b] + matrix[b][a];
			if (j != num_threads-1)
				f << ",";
		}
		f << endl;
	}
	f << endl;

	f.close();
}


void print_numa()
{
	int real_tid[MAXTHREADS+1];
	int i = 0;

	static long n = 0;
	ofstream f;
	char fname[255];

	sprintf(fname, "%06ld.page.csv", n++);

	cout << fname << endl;

	f.open(fname);

	for (auto it : pidmap)
		real_tid[it.second] = i++;

	f << "nr, addr, firstacc";
	for (int i = 0; i<num_threads; i++)
		f << ",T" << i;
	f << "\n";


	for(auto it : pagemap) {
		f << "0," << it.first << "," << real_tid[it.second[MAXTHREADS]-1];

		for (int i=0; i<num_threads; i++)
			f << "," << it.second[real_tid[i]];

		f << "\n";
	}

	f.close();
}



VOID Fini(INT32 code, VOID *v)
{
	if (DOCOMM)
		print_matrix();
	if (DOPAGE)
		print_numa();

	cout << endl << "MAXTHREADS: " << MAXTHREADS << " COMMSIZE: " << COMMSIZE << " PAGESIZE: " << PAGESIZE << " INTERVAL: " << INTERVAL << endl << endl;
}


int main(int argc, char *argv[])
{
	if (PIN_Init(argc,argv)) return 1;

	if (!DOCOMM && !DOPAGE) {
		cerr << "Error: need to choose at least one of communication (-c) or page usage (-p) detection" << endl;
    	cerr << endl << KNOB_BASE::StringKnobSummary() << endl;
    	return 1;
	}

	THREADID t = PIN_SpawnInternalThread(mythread, NULL, 0, NULL);
	if (t!=1)
		cerr << "ERROR " << t << endl;

	cout << endl << "MAXTHREADS: " << MAXTHREADS << " COMMSIZE: " << COMMSIZE << " PAGESIZE: " << PAGESIZE << " INTERVAL: " << INTERVAL << endl << endl;

	if (DOPAGE) {
		pagemap.reserve(3*4*1000000); // ~16GByte of mem usage, enough for NAS input C
		INS_AddInstrumentFunction(trace_memory_page, 0);
	}

	if (DOCOMM) {
		commmap.reserve(4*100000000);
		INS_AddInstrumentFunction(trace_memory_comm, 0);
	}

	PIN_AddThreadStartFunction(ThreadStart, 0);
	PIN_AddFiniFunction(Fini, 0);

	PIN_StartProgram();
}
