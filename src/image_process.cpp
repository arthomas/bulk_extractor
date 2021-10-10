/**
 * image_process.cpp:
 *
 * 64-bit file support.
 * Linux: See http://www.gnu.org/s/hello/manual/libc/Feature-Test-Macros.html
 *
 * MacOS & FreeBSD: Not needed; off_t is a 64-bit value.
 */

// Just for this module
#define _FILE_OFFSET_BITS 64

#include "config.h"

#include <algorithm>
#include <stdexcept>
#include <functional>
#include <locale>
#include <string>
#include <vector>

#ifdef HAVE_SYS_STAT_H
# include <sys/stat.h>
#endif

#ifdef HAVE_SYS_IOCTL_H
# include <sys/ioctl.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_SYS_FCNTL_H
#include <sys/fcntl.h>
#endif


#ifndef O_BINARY
#define O_BINARY 0
#endif

#ifndef PATH_MAX
#define PATH_MAX 65536
#endif

#include "be13_api/utf8.h"
#include "be13_api/utils.h"
#include "be13_api/formatter.h"
#include "image_process.h"



/****************************************************************
 ** patches for missing operating system calls.
 ****************************************************************/


/****************************************************************
 *** get_filesize()
 ****************************************************************/

/**
 * It's hard to figure out the filesize in an opearting system independent method that works with both
 * files and devices. This seems to work. It only requires a functioning pread64 or pread.
 */

#ifdef _WIN32
int pread64(HANDLE current_handle,char *buf,size_t bytes,uint64_t offset)
{
    DWORD bytes_read = 0;
    LARGE_INTEGER li;
    li.QuadPart = offset;
    li.LowPart = SetFilePointer(current_handle, li.LowPart, &li.HighPart, FILE_BEGIN);
    if(li.LowPart == INVALID_SET_FILE_POINTER){
        throw std::runtime_error("pread64: INVALID_FILE_SET_POINTER");
    }
    if (FALSE == ReadFile(current_handle, buf, (DWORD) bytes, &bytes_read, NULL)){
        throw std::runtime_error("pread64: ReadFile returned FALSE");
    }
    return bytes_read;
}
#else
#if !defined(HAVE_PREAD64) && !defined(HAVE_PREAD) && defined(HAVE__LSEEKI64)
static size_t pread64(int d,void *buf,size_t nbyte,int64_t offset)
{
    if(_lseeki64(d,offset,0)!=offset){
        throw std::runtime_error("_lseeki64 did not return offset");
    }
    return read(d,buf,nbyte);
}
#endif
#endif

#ifdef _WIN32
int64_t get_filesize(HANDLE fd)
#else
int64_t get_filesize(int fd)
#endif
{
    char buf[64];
    int64_t raw_filesize = 0;		/* needs to be signed for lseek */
    int bits = 0;
    int i =0;

#if defined(HAVE_PREAD64)
    /* If we have pread64, make sure it is defined */
    extern size_t pread64(int fd,char *buf,size_t nbyte,off_t offset);
#endif

#if !defined(HAVE_PREAD64) && defined(HAVE_PREAD)
    /* if we are not using pread64, make sure that off_t is 8 bytes in size */
#define pread64(d,buf,nbyte,offset) pread(d,buf,nbyte,offset)
    if(sizeof(off_t)!=8){
	throw std::runtime_error("Compiled with off_t!=8 no pread64 support.");
    }
#endif

#ifndef _WIN32
    /* We can use fstat if sizeof(st_size)==8 and st_size>0 */
    struct stat st;
    memset(&st,0,sizeof(st));
    if(sizeof(st.st_size)==8 && fstat(fd,&st)==0){
	if(st.st_size>0){
            return st.st_size;
        }
    }
#endif

    /* Phase 1; figure out how far we can seek... */
    for(bits=0;bits<60;bits++){
	raw_filesize = ((int64_t)1<<bits);
	if(::pread64(fd,buf,1,raw_filesize)!=1){
	    break;
	}
    }
    if(bits==60) throw std::runtime_error("Partition detection not functional.\n");

    /* Phase 2; blank bits as necessary */
    for(i=bits;i>=0;i--){
	int64_t test = (int64_t)1<<i;
	int64_t test_filesize = raw_filesize | ((int64_t)1<<i);
	if(::pread64(fd,buf,1,test_filesize)==1){
	    raw_filesize |= test;
	} else{
	    raw_filesize &= ~test;
	}
    }
    if(raw_filesize>0) raw_filesize+=1;	/* seems to be needed */
    return raw_filesize;
}


/****************************************************************
 *** static functions
 ****************************************************************/

std::string image_process::filename_extension(std::filesystem::path fn_)
{
    std::string fn(fn_.string());
    size_t dotpos = fn.rfind('.');
    if(dotpos==std::string::npos) return "";

    return fn.substr(dotpos+1);
}



image_process::image_process(std::filesystem::path fn, size_t pagesize_, size_t margin_):
    image_fname_(fn),pagesize(pagesize_),margin(margin_),report_read_errors(true)
{
}

image_process::~image_process()
{
}


std::filesystem::path image_process::image_fname() const
{
    return image_fname_;
}



bool image_process::fn_ends_with(std::filesystem::path path, std::string suffix)
{
    std::string str(path.string());
    if (suffix.size() > str.size()) return false;
    return str.substr(str.size()-suffix.size())==suffix;
}

bool image_process::is_multipart_file(std::filesystem::path fn)
{
    return fn_ends_with(fn,".000")
	|| fn_ends_with(fn,".001")
	|| fn_ends_with(fn,"001.vmdk");
}

/* fn can't be & because it will get modified */
std::string image_process::make_list_template(std::filesystem::path path_,int *start)
{
    /* First find where the digits are */
    std::string path(path_.string());
    size_t p = path.rfind("000");
    if(p==std::string::npos) p = path.rfind("001");
    assert(p!=std::string::npos);

    *start = atoi(path.substr(p,3).c_str()) + 1;
    path.replace(p,3,"%03d");	// make it a format
    return path;
}



/****************************************************************
 *** EWF START
 ****************************************************************/

/**
 * Works with both new API and old API
 */

#ifdef HAVE_LIBEWF
#ifdef libewf_handle_get_header_value_case_number
#define LIBEWFNG
#endif

/****************************************************************
 ** process_ewf
 */

void process_ewf::local_e01_glob(std::filesystem::path fname,char ***libewf_filenames,int *amount_of_filenames)
{
    std::cerr << "Experimental code for E01 names with MD5s appended\n";
#ifdef _WIN32
    /* Find the directory name */
    std::string dirname(fname);
    size_t pos = dirname.rfind("\\");                  // this this slash
    if(pos==std::string::npos) pos=dirname.rfind("/"); // try the other slash!
    if(pos!=std::string::npos){
        dirname.resize(pos+1);          // remove what's after the
    } else {
        dirname = "";                   // no directory?
    }

    /* Make the directory search template */
    char *buf = (char *)malloc(fname.size()+16);
    strcpy(buf,fname.c_str());
    /* Find the E01 */
    char *cc = strstr(buf,".E01.");
    if(!cc){
        throw image_process::NoSuchFile("Cannot find .E01. in filename");
    }
    for(;*cc;cc++){
        if(*cc!='.') *cc='?';          // replace the E01 and the MD5s at the end with ?s
    }
    std::wstring wbufstring = safe_utf8to16(buf); // convert to utf16
    const wchar_t *wbuf = wbufstring.c_str();

    /* Find the files */
    WIN32_FIND_DATA FindFileData;
    HANDLE hFind = FindFirstFile(wbuf, &FindFileData);
    if(hFind == INVALID_HANDLE_VALUE){
        throw std::runtime_error( Formatter() << "Invalid file pattern " << safe_utf16to8(wbufstring) );
    }
    std::vector<std::filesystem::path> files;
    files.push_back(dirname + safe_utf16to8(FindFileData.cFileName));
    while(FindNextFile(hFind,&FindFileData)!=0){
        files.push_back(dirname + safe_utf16to8(FindFileData.cFileName));
    }

    /* Sort the files */
    sort(files.begin(),files.end());

    /* Make the array */
    *amount_of_filenames = files.size();
    *libewf_filenames = (char **)calloc(sizeof(char *),files.size());
    for(size_t i=0;i<files.size();i++){
        (*libewf_filenames)[i] = strdup(files[i].c_str());
    }
    free((void *)buf);
#else
    std::cerr << "This code only runs on Windows.\n";
#endif
}


process_ewf::~process_ewf()
{
#ifdef HAVE_LIBEWF_HANDLE_CLOSE
    if(handle){
	libewf_handle_close(handle,NULL);
	libewf_handle_free(&handle,NULL);
    }
#else
    if(handle){
	libewf_close(handle);
    }
#endif
}


int process_ewf::open()
{
    std::filesystem::path fname = image_fname();
    std::string fname_string = fname.string();
    char **libewf_filenames = NULL;
    int amount_of_filenames = 0;

    std::cout << "Opening " << image_fname() << "... ";
    std::cout.flush();
#ifdef HAVE_LIBEWF_HANDLE_CLOSE
    bool use_libewf_glob = true;
    libewf_error_t *error=0;

    if(fname_string.find(".E01")!=std::string::npos){
        use_libewf_glob = false;
    }

    if(use_libewf_glob){
        if(libewf_glob(fname.c_str(), strlen(fname.c_str()), LIBEWF_FORMAT_UNKNOWN,
                       &libewf_filenames,&amount_of_filenames,&error)<0){
            libewf_error_fprint(error,stdout);
            libewf_error_free(&error);
            throw std::invalid_argument("libewf_glob");
        }
    } else {
        local_e01_glob(fname, &libewf_filenames,&amount_of_filenames);
        std::cerr << "amount of filenames=" << amount_of_filenames << "\n";
        for(int i=0;i<amount_of_filenames;i++){
            std::cerr << libewf_filenames[i] << "\n";
        }
    }
    handle = 0;
    if(libewf_handle_initialize(&handle,NULL)<0){
	throw image_process::NoSuchFile("Cannot initialize EWF handle?");
    }
    if(libewf_handle_open(handle,libewf_filenames,amount_of_filenames,
			  LIBEWF_OPEN_READ,&error)<0){
	if (error) libewf_error_fprint(error,stdout);
        for(size_t i = 0; libewf_filenames[i]; i++){
            std::cerr << "filename " << i << " = " << libewf_filenames[i] << "\n";
        }
	throw image_process::NoSuchFile( fname.string() );
    }
    /* Free the allocated filenames */
    if(use_libewf_glob){
        if(libewf_glob_free(libewf_filenames,amount_of_filenames,&error)<0){
            printf("libewf_glob_free failed\n");
            if(error) libewf_error_fprint(error,stdout);
            throw image_process::NoSuchFile("libewf_glob_free");
        }
    }
    libewf_handle_get_media_size(handle,(size64_t *)&ewf_filesize,NULL);
#else
    amount_of_filenames = libewf_glob(fname,strlen(fname),LIBEWF_FORMAT_UNKNOWN,&libewf_filenames);
    if(amount_of_filenames<0){
	err(1,"libewf_glob");
    }
    handle = libewf_open(libewf_filenames,amount_of_filenames,LIBEWF_OPEN_READ);
    if(handle==0){
	fprintf(stderr,"amount_of_filenames:%d\n",amount_of_filenames);
	for(int i=0;i<amount_of_filenames;i++){
	    fprintf(stderr,"  %s\n",libewf_filenames[i]);
	}
	throw image_process::NoSuchFile("libewf_open");
    }
    libewf_get_media_size(handle,(size64_t *)&ewf_filesize);
#endif

#ifdef HAVE_LIBEWF_HANDLE_GET_UTF8_HEADER_VALUE_NOTES
    uint8_t ewfbuf[65536];
    int status= libewf_handle_get_utf8_header_value_notes(handle, ewfbuf, sizeof(ewfbuf)-1, &error);
    if(status == 1 && strlen(ewfbuf)>0){
	std::string notes = reinterpret_cast<char *>(ewfbuf);
	details.push_back(std::string("NOTES: ")+notes);
    }

    status = libewf_handle_get_utf8_header_value_case_number(handle, ewfbuf, sizeof(ewfbuf)-1, &error);
    if(status == 1 && strlen(ewfbuf)>0){
	std::string case_number = reinterpret_cast<char *>(ewfbuf);
	details.push_back(std::string("CASE NUMBER: ")+case_number);
    }

    status = libewf_handle_get_utf8_header_value_evidence_number(handle, ewfbuf, sizeof(ewfbuf)-1, &error);
    if(status == 1 && strlen(ewfbuf)>0){
	std::string evidenceno = reinterpret_cast<char *>(ewfbuf);
	details.push_back(std::string("EVIDENCE NUMBER: ")+evidenceno);
    }

    status = libewf_handle_get_utf8_header_value_examiner_name(handle, ewfbuf, sizeof(ewfbuf)-1, &error);
    if(status == 1 && strlen(ewfbuf)>0){
	std::string examinername = reinterpret_cast<char *>(ewfbuf) ;
	details.push_back(std::string("EXAMINER NAME: "+examinername));
    }
#endif
    std::cout << "\r                                                                      " << std::endl;
    return 0;
}

std::vector<std::string> process_ewf::getewfdetails() const{
    return(details);
}


//int process_ewf::debug = 0;
ssize_t process_ewf::pread(void *buf,size_t bytes,uint64_t offset) const
{
#ifdef HAVE_LIBEWF_HANDLE_CLOSE
    libewf_error_t *error=0;
#if defined(HAVE_LIBEWF_HANDLE_READ_RANDOM)
    int ret = libewf_handle_read_random(handle,buf,bytes,offset,&error);
#endif
#if defined(HAVE_LIBEWF_HANDLE_READ_BUFFER_AT_OFFSET) && !defined(HAVE_LIBEWF_HANDLE_READ_RANDOM)
    int ret = libewf_handle_read_buffer_at_offset(handle,buf,bytes,offset,&error);
#endif
    if(ret<0){
	if (report_read_errors) libewf_error_fprint(error,stderr);
	libewf_error_free(&error);
    }
    return ret;
#else
    if((int64_t)bytes+offset > (int64_t)ewf_filesize){
	bytes = ewf_filesize - offset;
    }
    return libewf_read_random(handle,buf,bytes,offset);
#endif
}

int64_t process_ewf::image_size() const
{
    return ewf_filesize;
}


image_process::iterator process_ewf::begin() const
{
    image_process::iterator it(this);
    it.raw_offset = 0;
    return it;
}


image_process::iterator process_ewf::end() const
{
    image_process::iterator it(this);
    it.raw_offset = this->ewf_filesize;
    it.eof = true;
    return it;
}

pos0_t process_ewf::get_pos0(const image_process::iterator &it) const
{
    return pos0_t("",it.raw_offset);
}

/** Read from the iterator into a newly allocated sbuf */
sbuf_t *process_ewf::sbuf_alloc(image_process::iterator &it) const
{
    size_t count = pagesize + margin;
    size_t this_pagesize = pagesize;

    if(this->ewf_filesize < it.raw_offset + count){    /* See if that's more than I need */
	count = this->ewf_filesize - it.raw_offset;
    }

    if (this_pagesize > count ) {
        this_pagesize = count;
    }

    auto sbuf = sbuf_t::sbuf_malloc(get_pos0(it), count, this_pagesize);
    unsigned char *buf = static_cast<unsigned char *>(sbuf->malloc_buf());
    int count_read = this->pread(buf, count, it.raw_offset);
    if(count_read<0){
        delete sbuf;
	throw read_error();
    }
    if(count==0){
        delete sbuf;
	it.eof = true;
	return 0;
    }
    return sbuf;
}

/**
 * just add the page size for process_ewf
 */
void process_ewf::increment_iterator(image_process::iterator &it) const
{
    it.raw_offset += pagesize;
    if(it.raw_offset > this->ewf_filesize) it.raw_offset = this->ewf_filesize;
}

double process_ewf::fraction_done(const image_process::iterator &it) const
{
    return (double)it.raw_offset / (double)this->ewf_filesize;
}

std::string process_ewf::str(const image_process::iterator &it) const
{
    char buf[64];
    snprintf(buf,sizeof(buf),"Offset %" PRId64 "MB",it.raw_offset/1000000);
    return std::string(buf);
}

uint64_t process_ewf::max_blocks(const image_process::iterator &it) const
{
  return this->ewf_filesize / pagesize;
}

uint64_t process_ewf::seek_block(image_process::iterator &it,uint64_t block) const
{
    it.raw_offset = pagesize * block;
    return block;
}
#endif



/****************************************************************
 *** RAW
 ****************************************************************/

/**
 * process a raw, with the appropriate threading.
 */

/****************************************************************
 * process_raw
 */

process_raw::process_raw(std::filesystem::path fname, size_t pagesize_, size_t margin_)
    :image_process(fname, pagesize_, margin_),
     file_list(),raw_filesize(0),current_file_name(),
#ifdef _WIN32
     current_handle(INVALID_HANDLE_VALUE)
#else
     current_fd(-1)
#endif
{
}

process_raw::~process_raw()
{
#ifdef _WIN32
    if(current_handle!=INVALID_HANDLE_VALUE) ::CloseHandle(current_handle);
#else
    if(current_fd>0) ::close(current_fd);
#endif
}

#ifdef _WIN32
BOOL GetDriveGeometry(const wchar_t *wszPath, DISK_GEOMETRY *pdg)
{
    HANDLE hDevice = INVALID_HANDLE_VALUE;  // handle to the drive to be examined
    BOOL bResult   = FALSE;                 // results flag
    DWORD junk     = 0;                     // discard results

    hDevice = CreateFileW(wszPath,          // drive to open
                          0,                // no access to the drive
                          FILE_SHARE_READ | // share mode
                          FILE_SHARE_WRITE,
                          NULL,             // default security attributes
                          OPEN_EXISTING,    // disposition
                          0,                // file attributes
                          NULL);            // do not copy file attributes

    if (hDevice == INVALID_HANDLE_VALUE){    // cannot open the drive
        throw image_process::NoSuchFile("GetDriveGeometry: Cannot open drive");
    }

    bResult = DeviceIoControl(hDevice,                       // device to be queried
                              IOCTL_DISK_GET_DRIVE_GEOMETRY, // operation to perform
                              NULL, 0,                       // no input buffer
                              pdg, sizeof(*pdg),            // output buffer
                              &junk,                         // # bytes returned
                              (LPOVERLAPPED) NULL);          // synchronous I/O

    CloseHandle(hDevice);

    return (bResult);
}

#endif

/**
 * Add the file to the list, keeping track of the total size
 */
void process_raw::add_file(std::filesystem::path fname)
{
    int64_t fname_filesize = std::filesystem::file_size(fname);

#ifdef _WIN32
    if (fname_filesize==0){
        /* On Windows, see if we can use this */
        fprintf(stderr,"%s checking physical drive\n",fname.c_str());
        // http://msdn.microsoft.com/en-gb/library/windows/desktop/aa363147%28v=vs.85%29.aspx
        DISK_GEOMETRY pdg = { 0 }; // disk drive geometry structure
        std::wstring wszDrive = safe_utf8to16(fname.string());
        GetDriveGeometry(wszDrive.c_str(), &pdg);
        fname_filesize = pdg.Cylinders.QuadPart * (ULONG)pdg.TracksPerCylinder *
            (ULONG)pdg.SectorsPerTrack * (ULONG)pdg.BytesPerSector;
    }
#endif
    file_list.push_back(file_info(fname,raw_filesize,fname_filesize));
    raw_filesize += fname_filesize;
}

const process_raw::file_info *process_raw::find_offset(uint64_t pos) const
{
    for(process_raw::file_list_t::const_iterator it = file_list.begin();it != file_list.end();it++){
	if((*it).offset<=pos && pos< ((*it).offset+(*it).length)){
	    return &(*it);
	}
    }
    return 0;
}

/**
 * Open the first image and, optionally, all of the others.
 */
int process_raw::open()
{
    add_file(image_fname());

    /* Get the list of the files if this is a split-raw file */
    if(is_multipart_file(image_fname())){
	int num=0;
        std::string templ = make_list_template(image_fname(),&num);
	for(;;num++){
	    char probename[PATH_MAX];
	    snprintf(probename,sizeof(probename),templ.c_str(),num);
	    if(access(probename,R_OK)!=0) break;	    // no more files
	    add_file(std::filesystem::path(probename)); // found another name
	}
    }
    return 0;
}

int64_t process_raw::image_size() const
{
    return raw_filesize;
}


/**
 * Read randomly between a split file.
 * 1. Determine which file to read and how many bytes from that file can be read.
 * 2. Perform the read.
 * 3. If there are additional files to read in the next file, recurse.
 */

ssize_t process_raw::pread(void *buf, size_t bytes, uint64_t offset) const
{
    const file_info *fi = find_offset(offset);
    if(fi==0) return 0;			// nothing to read.

    /* See if the file is the one that's currently opened.
     * If not, close the current one and open the new one.
     */

    if(fi->name != current_file_name){
#ifdef _WIN32
        if(current_handle!=INVALID_HANDLE_VALUE) ::CloseHandle(current_handle);
#else
	if(current_fd>=0) close(current_fd);
#endif

	current_file_name = fi->name;
#ifdef _WIN32
        std::wstring path16 = safe_utf8to16(fi->name.string());
        current_handle = CreateFileA(reinterpret_cast<const char *>(path16.c_str()), FILE_READ_DATA,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
				     OPEN_EXISTING, 0, NULL);
        if(current_handle==INVALID_HANDLE_VALUE){
            throw image_process::NoSuchFile("pread: INVALID_HANDLE_VALUE");
	}
#else
	current_fd = ::open(fi->name.c_str(),O_RDONLY|O_BINARY);
	if(current_fd<=0){
            std::cerr << "process_raw::pread: cannot open " << fi->name << "\n";
            throw image_process::NoSuchFile("pread: Cannot ::open file");
        }
#endif
    }

#if defined(HAVE_PREAD64)
    /* If we have pread64, make sure it is defined */
    extern size_t pread64(int fd,char *buf,size_t nbyte,off_t offset);
#endif

#if !defined(HAVE_PREAD64) && defined(HAVE_PREAD)
    /* if we are not using pread64, make sure that off_t is 8 bytes in size */
#define pread64(d,buf,nbyte,offset) pread(d,buf,nbyte,offset)
#endif

    /* we have neither, so just hack it with lseek64 */
    assert(fi->offset <= offset);
#ifdef _WIN32
    DWORD bytes_read = 0;
    LARGE_INTEGER li;
    li.QuadPart = offset - fi->offset;
    li.LowPart = SetFilePointer(current_handle, li.LowPart, &li.HighPart, FILE_BEGIN);
    if(li.LowPart == INVALID_SET_FILE_POINTER) return -1;
    if (FALSE == ReadFile(current_handle, buf, (DWORD) bytes, &bytes_read, NULL)){
        throw image_process::NoSuchFile("pread: INVALID_FILE_SET_POINTER");
    }
#else
    ssize_t bytes_read = ::pread64(current_fd,buf,bytes,offset - fi->offset);
#endif
    if(bytes_read<0){
        throw image_process::NoSuchFile("pread64: READ LESS THAN 0 BYTES");
    }
    if((size_t)bytes_read==bytes) return bytes_read; // read precisely the correct amount!

    /* Need to recurse */
    ssize_t bytes_read2 = this->pread(static_cast<char *>(buf)+bytes_read,bytes-bytes_read,offset+bytes_read);
    if(bytes_read2<0) return -1;	// error on second read
    if(bytes_read==0) return 0;		// kind of odd.

    return bytes_read + bytes_read2;
}


image_process::iterator process_raw::begin() const
{
    image_process::iterator it(this);
    return it;
}


/* Returns an iterator at the end of the image */
image_process::iterator process_raw::end() const
{
    image_process::iterator it(this);
    it.raw_offset = this->raw_filesize;
    it.eof = true;
    return it;
}

void process_raw::increment_iterator(image_process::iterator &it) const
{
    it.raw_offset += pagesize;
    if (it.raw_offset > this->raw_filesize) it.raw_offset = this->raw_filesize;
}

double process_raw::fraction_done(const image_process::iterator &it) const
{
    return (double)it.raw_offset / (double)this->raw_filesize;
}

std::string process_raw::str(const image_process::iterator &it) const
{
    char buf[64];
    snprintf(buf,sizeof(buf),"Offset %" PRId64 "MB",it.raw_offset/1000000);
    return std::string(buf);
}


pos0_t process_raw::get_pos0(const image_process::iterator &it) const
{
    return pos0_t("",it.raw_offset);
}

/** Read from the iterator into a newly allocated sbuf.
 * uses pagesize. We don't memory map the file. Perhaps we should. But then we could only do 4K pages.
 */
sbuf_t *process_raw::sbuf_alloc(image_process::iterator &it) const
{
    size_t count = pagesize + margin;
    size_t this_pagesize = pagesize;

    if(this->raw_filesize < it.raw_offset + count){    /* See if that's more than I need */
	count = this->raw_filesize - it.raw_offset;
    }
    if (this_pagesize > count ) {
        this_pagesize = count;
    }

    sbuf_t *sbuf = sbuf_t::sbuf_malloc( get_pos0(it), count, this_pagesize);
    unsigned char *buf = reinterpret_cast<unsigned char *>(sbuf->malloc_buf());
    int count_read = this->pread(buf, count, it.raw_offset);       // do the read
    if (count_read==0){
        delete sbuf;
	it.eof = true;
        throw EndOfImage();
    }
    if(count_read<0){
        delete sbuf;
	throw read_error();
    }
    return sbuf;
}

uint64_t process_raw::max_blocks(const image_process::iterator &it) const
{
    return (this->raw_filesize+pagesize-1) / pagesize;
}

uint64_t process_raw::seek_block(image_process::iterator &it,uint64_t block) const
{
    if(block * pagesize > (uint64_t)raw_filesize){
        block = raw_filesize / pagesize;
    }

    it.raw_offset = block * pagesize;
    return block;
}


/****************************************************************
 *** Directory Recursion
 ****************************************************************/

/**
 * directories don't get page sizes or margins; the page size is the entire
 * file and the margin is 0.
 */
process_dir::process_dir(std::filesystem::path image_dir): image_process(image_dir,0,0)
{
    for (const auto& entry : std::filesystem::recursive_directory_iterator( image_dir )) {
        if (entry.is_regular_file()) {
            files.push_back( entry );
        }
    }
}

process_dir::~process_dir()
{
}

int process_dir::open()
{
    return 0;				// always successful
}

ssize_t process_dir::pread(void *buf,size_t bytes,uint64_t offset) const
{
    throw std::runtime_error("process_dir does not support pread");
}

int64_t process_dir::image_size() const
{
    return files.size();		// the 'size' is in files
}

image_process::iterator process_dir::begin() const
{
    image_process::iterator it(this);
    return it;
}

image_process::iterator process_dir::end() const
{
    image_process::iterator it(this);
    it.file_number = files.size();
    it.eof = true;
    return it;
}

void process_dir::increment_iterator(image_process::iterator &it) const
{
    it.file_number++;
    if(it.file_number>files.size()) it.file_number=files.size();
}

#pragma GCC diagnostic ignored "-Wsuggest-attribute=noreturn"
pos0_t process_dir::get_pos0(const image_process::iterator &it) const
{
    return pos0_t(files[it.file_number].string(), 0);
}
#pragma GCC diagnostic warning "-Wsuggest-attribute=noreturn"

/** Read from the iterator into a newly allocated sbuf
 * with mapped memory.
 */
sbuf_t *process_dir::sbuf_alloc(image_process::iterator &it) const
{
    std::filesystem::path fname = files[it.file_number];
    sbuf_t *sbuf = sbuf_t::map_file(fname);     // returns a new sbuf
    return sbuf;
}

double process_dir::fraction_done(const image_process::iterator &it) const
{
    return (double)it.file_number / (double)files.size();
}

std::string process_dir::str(const image_process::iterator &it) const
{
    return std::string("File ") + files[it.file_number].string();
}


uint64_t process_dir::max_blocks(const image_process::iterator &it) const
{
    return files.size();
}

uint64_t process_dir::seek_block(class image_process::iterator &it,uint64_t block) const
{
    it.file_number = block;
    return it.file_number;
}



/****************************************************************
 *** COMMON - Implement 'open' for the iterator
 ****************************************************************/
/* Static function */

image_process *image_process::open(std::filesystem::path fn, bool opt_recurse, size_t pagesize_, size_t margin_)
{
    image_process *ip = 0;
    std::string ext = filename_extension(fn);
    struct stat st;
    bool  is_windows_unc = false;
    std::string fname_string = fn.string();

#ifdef _WIN32
    if(fname_string.size()>2 && fname_string[0]=='\\' && fname_string[1]=='\\') is_windows_unc=true;
#endif

    memset(&st,0,sizeof(st));
    if (stat(fname_string.c_str(),&st) && !is_windows_unc){
	throw NoSuchFile(fname_string);
    }
    if(S_ISDIR(st.st_mode)){
	/* If this is a directory, process specially */
	if(opt_recurse==0){
	    std::cerr << "error: " << fname_string << " is a directory but -R (opt_recurse) not set\n";
	    errno = 0;
	    throw NoSuchFile(fname_string);	// directory and cannot recurse
	}
        /* Quickly scan the directory and see if it has a .E01, .000 or .001 file.
         * If so, give the user an error.
         */
        for( const auto &p : std::filesystem::directory_iterator( fn )){
            if ( p.path().extension()==".E01" ||
                 p.path().extension()==".000" ||
                 p.path().extension()==".001") {
                std::cerr << "error: file " << p.path() << " is in directory " << fn << "\n";
                std::cerr << "       The -R option is not for reading a directory of EnCase files\n";
                std::cerr << "       or a directory of disk image parts. Please process these\n";
                std::cerr << "       as a single disk image. If you need to process these files\n";
                std::cerr << "       then place them in a sub directory of " << fn << "\n";
                throw NoSuchFile( fname_string );
            }
        }
	ip = new process_dir(fn);
    }
    else {
	/* Otherwise open a file by checking extension.
	 *
	 * I would rather use the localized version at
	 * http://stackoverflow.com/questions/313970/stl-string-to-lower-case
	 * but it generates a compile-time error.
	 */

	std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

	if(ext=="e01" || fname_string.find(".E01.")!=std::string::npos){
#ifdef HAVE_LIBEWF
	    ip = new process_ewf(fn,pagesize_,margin_);
#else
	    throw NoSupport("This program was compiled without E01 support");
#endif
	}
	if(!ip) ip = new process_raw(fn,pagesize_,margin_);
    }
    /* Try to open it */
    if (ip->open()){
        throw NoSuchFile(fname_string);
    }
    return ip;
}
