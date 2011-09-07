/*
 * makedumpfile.c
 *
 * Copyright (C) 2006, 2007, 2008, 2009  NEC Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include "makedumpfile.h"
#include "print_info.h"
#include "dwarf_info.h"
#include "elf_info.h"
#include <sys/time.h>

struct symbol_table	symbol_table;
struct size_table	size_table;
struct offset_table	offset_table;
struct array_table	array_table;
struct number_table	number_table;
struct srcfile_table	srcfile_table;

struct vm_table		vt = { 0 };
struct DumpInfo		*info = NULL;
struct module_sym_table	mod_st = { 0 };
struct filter_info	*filter_info = NULL;
struct filter_config	filter_config;
struct erase_info	*erase_info = NULL;
unsigned long		num_erase_info = 1; /* Node 0 is unused. */

char filename_stdout[] = FILENAME_STDOUT;
char config_buf[BUFSIZE_FGETS];

/*
 * The numbers of the excluded pages
 */
unsigned long long pfn_zero;
unsigned long long pfn_memhole;
unsigned long long pfn_cache;
unsigned long long pfn_cache_private;
unsigned long long pfn_user;
unsigned long long pfn_free;

int retcd = FAILED;	/* return code */

#define INITIALIZE_LONG_TABLE(table, value) \
do { \
	size_member = sizeof(long); \
	num_member  = sizeof(table) / size_member; \
	ptr_long_table = (long *)&table; \
	for (i = 0; i < num_member; i++, ptr_long_table++) \
		*ptr_long_table = value; \
} while (0)

void
initialize_tables(void)
{
	int i, size_member, num_member;
	unsigned long long *ptr_symtable;
	long *ptr_long_table;

	/*
	 * Initialize the symbol table.
	 */
	size_member = sizeof(symbol_table.mem_map);
	num_member  = sizeof(symbol_table) / size_member;

	ptr_symtable = (unsigned long long *)&symbol_table;

	for (i = 0; i < num_member; i++, ptr_symtable++)
		*ptr_symtable = NOT_FOUND_SYMBOL;

	INITIALIZE_LONG_TABLE(size_table, NOT_FOUND_STRUCTURE);
	INITIALIZE_LONG_TABLE(offset_table, NOT_FOUND_STRUCTURE);
	INITIALIZE_LONG_TABLE(array_table, NOT_FOUND_STRUCTURE);
	INITIALIZE_LONG_TABLE(number_table, NOT_FOUND_NUMBER);
}

/*
 * Translate a domain-0's physical address to machine address.
 */
unsigned long long
ptom_xen(unsigned long long paddr)
{
	unsigned long mfn;
	unsigned long long maddr, pfn, mfn_idx, frame_idx;

	pfn = paddr_to_pfn(paddr);
	mfn_idx   = pfn / MFNS_PER_FRAME;
	frame_idx = pfn % MFNS_PER_FRAME;

	if (mfn_idx >= info->p2m_frames) {
		ERRMSG("Invalid mfn_idx(%llu).\n", mfn_idx);
		return NOT_PADDR;
	}
	maddr = pfn_to_paddr(info->p2m_mfn_frame_list[mfn_idx])
		+ sizeof(unsigned long) * frame_idx;
	if (!readmem(MADDR_XEN, maddr, &mfn, sizeof(mfn))) {
		ERRMSG("Can't get mfn.\n");
		return NOT_PADDR;
	}
	maddr  = pfn_to_paddr(mfn);
	maddr |= PAGEOFFSET(paddr);

	return maddr;
}

/*
 * Get the number of the page descriptors from the ELF info.
 */
int
get_max_mapnr(void)
{
	unsigned long long max_paddr;

	if (info->flag_refiltering) {
		info->max_mapnr = info->dh_memory->max_mapnr;
		return TRUE;
	}
	max_paddr = get_max_paddr();
	info->max_mapnr = paddr_to_pfn(max_paddr);

	DEBUG_MSG("\n");
	DEBUG_MSG("max_mapnr    : %llx\n", info->max_mapnr);

	return TRUE;
}

/*
 * Get the number of the page descriptors for Xen.
 */
int
get_dom0_mapnr()
{
	unsigned long max_pfn;

	if (SYMBOL(max_pfn) == NOT_FOUND_SYMBOL)
		return FALSE;

	if (!readmem(VADDR, SYMBOL(max_pfn), &max_pfn, sizeof max_pfn))
		return FALSE;

	info->dom0_mapnr = max_pfn;

	return TRUE;
}

int
is_in_same_page(unsigned long vaddr1, unsigned long vaddr2)
{
	if (round(vaddr1, info->page_size) == round(vaddr2, info->page_size))
		return TRUE;

	return FALSE;
}

#define BITMAP_SECT_LEN 4096
static inline int is_dumpable(struct dump_bitmap *, unsigned long long);
unsigned long
pfn_to_pos(unsigned long long pfn)
{
	unsigned long desc_pos, i;

	desc_pos = info->valid_pages[pfn / BITMAP_SECT_LEN];
	for (i = round(pfn, BITMAP_SECT_LEN); i < pfn; i++)
		if (is_dumpable(info->bitmap_memory, i))
			desc_pos++;

	return desc_pos;
}

int
read_page_desc(unsigned long long paddr, page_desc_t *pd)
{
	struct disk_dump_header *dh;
	unsigned long desc_pos;
	unsigned long long pfn;
	off_t offset;

	/*
	 * Find page descriptor
	 */
	dh = info->dh_memory;
	offset
	    = (DISKDUMP_HEADER_BLOCKS + dh->sub_hdr_size + dh->bitmap_blocks)
		* dh->block_size;
	pfn = paddr_to_pfn(paddr);
	desc_pos = pfn_to_pos(pfn);
	offset += (off_t)desc_pos * sizeof(page_desc_t);
	if (lseek(info->fd_memory, offset, SEEK_SET) < 0) {
		ERRMSG("Can't seek %s. %s\n",
				 info->name_memory, strerror(errno));
		return FALSE;
	}

	/*
	 * Read page descriptor
	 */
	if (read(info->fd_memory, pd, sizeof(*pd)) != sizeof(*pd)) {
		ERRMSG("Can't read %s. %s\n",
				info->name_memory, strerror(errno));
		return FALSE;
	}

	/*
	 * Sanity check
	 */
	if (pd->size > dh->block_size)
		return FALSE;

	return TRUE;
}

int
readpmem_kdump_compressed(unsigned long long paddr, void *bufptr, size_t size)
{
	page_desc_t pd;
	char buf[info->page_size];
	char buf2[info->page_size];
	int ret;
	unsigned long retlen, page_offset;

	page_offset = paddr % info->page_size;

	if (!is_dumpable(info->bitmap_memory, paddr_to_pfn(paddr))) {
		ERRMSG("pfn(%llx) is excluded from %s.\n",
				paddr_to_pfn(paddr), info->name_memory);
		goto error;
	}

	if (!read_page_desc(paddr, &pd)) {
		ERRMSG("Can't read page_desc: %llx\n", paddr);
		goto error;
	}

	if (lseek(info->fd_memory, pd.offset, SEEK_SET) < 0) {
		ERRMSG("Can't seek %s. %s\n",
				info->name_memory, strerror(errno));
		goto error;
	}

	/*
	 * Read page data
	 */
	if (read(info->fd_memory, buf, pd.size) != pd.size) {
		ERRMSG("Can't read %s. %s\n",
				info->name_memory, strerror(errno));
		goto error;
	}

	if (pd.flags & DUMP_DH_COMPRESSED) {
		retlen = info->page_size;
		ret = uncompress((unsigned char *)buf2, &retlen,
					(unsigned char *)buf, pd.size);
		if ((ret != Z_OK) || (retlen != info->page_size)) {
			ERRMSG("Uncompress failed: %d\n", ret);
			goto error;
		}
		memcpy(bufptr, buf2 + page_offset, size);
	} else
		memcpy(bufptr, buf + page_offset, size);

	return size;
error:
	ERRMSG("type_addr: %d, addr:%llx, size:%zd\n", PADDR, paddr, size);
	return FALSE;
}

int
readmem(int type_addr, unsigned long long addr, void *bufptr, size_t size)
{
	size_t read_size, next_size;
	off_t offset = 0;
	unsigned long long next_addr;
	unsigned long long paddr, maddr = NOT_PADDR;
	char *next_ptr;
	const off_t failed = (off_t)-1;

	switch (type_addr) {
	case VADDR:
		if ((paddr = vaddr_to_paddr(addr)) == NOT_PADDR) {
			ERRMSG("Can't convert a virtual address(%llx) to physical address.\n",
			    addr);
			goto error;
		}
		if (is_xen_memory()) {
			if ((maddr = ptom_xen(paddr)) == NOT_PADDR) {
				ERRMSG("Can't convert a physical address(%llx) to machine address.\n",
				    paddr);
				return FALSE;
			}
			paddr = maddr;
		}
		break;
	case PADDR:
		paddr = addr;
		if (is_xen_memory()) {
			if ((maddr  = ptom_xen(paddr)) == NOT_PADDR) {
				ERRMSG("Can't convert a physical address(%llx) to machine address.\n",
				    paddr);
				return FALSE;
			}
			paddr = maddr;
		}
		break;
	case VADDR_XEN:
		if ((paddr = kvtop_xen(addr)) == NOT_PADDR) {
			ERRMSG("Can't convert a virtual address(%llx) to machine address.\n",
			    addr);
			goto error;
		}
		break;
	case MADDR_XEN:
		paddr = addr;
  		break;
	default:
		ERRMSG("Invalid address type (%d).\n", type_addr);
		goto error;
	}

	read_size = size;

	/*
	 * Read each page, because pages are not necessarily continuous.
	 * Ex) pages in vmalloc area
	 */
	if (!is_in_same_page(addr, addr + size - 1)) {
		read_size = info->page_size - (addr % info->page_size);
		next_addr = roundup(addr + 1, info->page_size);
		next_size = size - read_size;
		next_ptr  = (char *)bufptr + read_size;

		if (!readmem(type_addr, next_addr, next_ptr, next_size))
			goto error;
	}

	if (info->flag_refiltering)
		return readpmem_kdump_compressed(paddr, bufptr, read_size);

	if (!(offset = paddr_to_offset(paddr))) {
		ERRMSG("Can't convert a physical address(%llx) to offset.\n",
		    paddr);
		goto error;
	}

	if (lseek(info->fd_memory, offset, SEEK_SET) == failed) {
		ERRMSG("Can't seek the dump memory(%s). (offset: %llx) %s\n",
		    info->name_memory, (unsigned long long)offset, strerror(errno));
		goto error;
	}

	if (read(info->fd_memory, bufptr, read_size) != read_size) {
		ERRMSG("Can't read the dump memory(%s). %s\n",
		    info->name_memory, strerror(errno));
		goto error;
	}

	return size;
error:
	ERRMSG("type_addr: %d, addr:%llx, size:%zd\n", type_addr, addr, size);
	return FALSE;
}

int32_t
get_kernel_version(char *release)
{
	int32_t version;
	long maj, min, rel;
	char *start, *end;

	/*
	 * This method checks that vmlinux and vmcore are same kernel version.
	 */
	start = release;
	maj = strtol(start, &end, 10);
	if (maj == LONG_MAX)
		return FALSE;

	start = end + 1;
	min = strtol(start, &end, 10);
	if (min == LONG_MAX)
		return FALSE;

	start = end + 1;
	rel = strtol(start, &end, 10);
	if (rel == LONG_MAX)
  		return FALSE;

	version = KERNEL_VERSION(maj, min, rel);

	if ((version < OLDEST_VERSION) || (LATEST_VERSION < version)) {
		MSG("The kernel version is not supported.\n");
		MSG("The created dumpfile may be incomplete.\n");
	}
	return version;
}

int
is_page_size(long page_size)
{
	/*
	 * Page size is restricted to a hamming weight of 1.
	 */
	if (page_size > 0 && !(page_size & (page_size - 1)))
		return TRUE;

	return FALSE;
}

int
set_page_size(long page_size)
{
	if (!is_page_size(page_size)) {
		ERRMSG("Invalid page_size: %ld", page_size);
		return FALSE;
	}
	info->page_size = page_size;
	info->page_shift = ffs(info->page_size) - 1;
	DEBUG_MSG("page_size    : %ld\n", info->page_size);

	return TRUE;
}

int
fallback_to_current_page_size(void)
{

	if (!set_page_size(sysconf(_SC_PAGE_SIZE)))
		return FALSE;

	DEBUG_MSG("WARNING: Cannot determine page size (no vmcoreinfo).\n");
	DEBUG_MSG("Using the dump kernel page size: %ld\n",
	    info->page_size);

	return TRUE;
}

int
check_release(void)
{
	unsigned long utsname;

	/*
	 * Get the kernel version.
	 */
	if (SYMBOL(system_utsname) != NOT_FOUND_SYMBOL) {
		utsname = SYMBOL(system_utsname);
	} else if (SYMBOL(init_uts_ns) != NOT_FOUND_SYMBOL) {
		utsname = SYMBOL(init_uts_ns) + sizeof(int);
	} else {
		ERRMSG("Can't get the symbol of system_utsname.\n");
		return FALSE;
	}
	if (!readmem(VADDR, utsname, &info->system_utsname,
					sizeof(struct utsname))) {
		ERRMSG("Can't get the address of system_utsname.\n");
		return FALSE;
	}

	if (info->flag_read_vmcoreinfo) {
		if (strcmp(info->system_utsname.release, info->release)) {
			ERRMSG("%s and %s don't match.\n",
			    info->name_vmcoreinfo, info->name_memory);
			retcd = WRONG_RELEASE;
			return FALSE;
		}
	}

	info->kernel_version = get_kernel_version(info->system_utsname.release);
	if (info->kernel_version == FALSE) {
		ERRMSG("Can't get the kernel version.\n");
		return FALSE;
	}

	return TRUE;
}

int
open_vmcoreinfo(char *mode)
{
	FILE *file_vmcoreinfo;

	if ((file_vmcoreinfo = fopen(info->name_vmcoreinfo, mode)) == NULL) {
		ERRMSG("Can't open the vmcoreinfo file(%s). %s\n",
		    info->name_vmcoreinfo, strerror(errno));
		return FALSE;
	}
	info->file_vmcoreinfo = file_vmcoreinfo;
	return TRUE;
}

int
open_kernel_file(void)
{
	int fd;

	if (info->name_vmlinux) {
		if ((fd = open(info->name_vmlinux, O_RDONLY)) < 0) {
			ERRMSG("Can't open the kernel file(%s). %s\n",
			    info->name_vmlinux, strerror(errno));
			return FALSE;
		}
		info->fd_vmlinux = fd;
	}
	if (info->name_xen_syms) {
		if ((fd = open(info->name_xen_syms, O_RDONLY)) < 0) {
			ERRMSG("Can't open the kernel file(%s). %s\n",
			    info->name_xen_syms, strerror(errno));
			return FALSE;
		}
		info->fd_xen_syms = fd;
	}
	return TRUE;
}

int
check_kdump_compressed(char *filename)
{
	struct disk_dump_header dh;

	if (!__read_disk_dump_header(&dh, filename))
		return ERROR;

	if (strncmp(dh.signature, KDUMP_SIGNATURE, SIG_LEN))
		return FALSE;

	return TRUE;
}

int
get_kdump_compressed_header_info(char *filename)
{
	struct disk_dump_header dh;
	struct kdump_sub_header kh;

	if (!read_disk_dump_header(&dh, filename))
		return FALSE;

	if (!read_kdump_sub_header(&kh, filename))
		return FALSE;

	if (dh.header_version < 1) {
		ERRMSG("header does not have dump_level member\n");
		return FALSE;
	}
	DEBUG_MSG("diskdump main header\n");
	DEBUG_MSG("  signature        : %s\n", dh.signature);
	DEBUG_MSG("  header_version   : %d\n", dh.header_version);
	DEBUG_MSG("  status           : %d\n", dh.status);
	DEBUG_MSG("  block_size       : %d\n", dh.block_size);
	DEBUG_MSG("  sub_hdr_size     : %d\n", dh.sub_hdr_size);
	DEBUG_MSG("  bitmap_blocks    : %d\n", dh.bitmap_blocks);
	DEBUG_MSG("  max_mapnr        : 0x%x\n", dh.max_mapnr);
	DEBUG_MSG("  total_ram_blocks : %d\n", dh.total_ram_blocks);
	DEBUG_MSG("  device_blocks    : %d\n", dh.device_blocks);
	DEBUG_MSG("  written_blocks   : %d\n", dh.written_blocks);
	DEBUG_MSG("  current_cpu      : %d\n", dh.current_cpu);
	DEBUG_MSG("  nr_cpus          : %d\n", dh.nr_cpus);
	DEBUG_MSG("kdump sub header\n");
	DEBUG_MSG("  phys_base        : 0x%lx\n", kh.phys_base);
	DEBUG_MSG("  dump_level       : %d\n", kh.dump_level);
	DEBUG_MSG("  split            : %d\n", kh.split);
	DEBUG_MSG("  start_pfn        : 0x%lx\n", kh.start_pfn);
	DEBUG_MSG("  end_pfn          : 0x%lx\n", kh.end_pfn);

	info->dh_memory = malloc(sizeof(dh));
	if (info->dh_memory == NULL) {
		ERRMSG("Can't allocate memory for the header. %s\n",
		    strerror(errno));
		return FALSE;
	}
	memcpy(info->dh_memory, &dh, sizeof(dh));
	memcpy(&info->timestamp, &dh.timestamp, sizeof(dh.timestamp));

	info->kh_memory = malloc(sizeof(kh));
	if (info->kh_memory == NULL) {
		ERRMSG("Can't allocate memory for the sub header. %s\n",
		    strerror(errno));
		goto error;
	}
	memcpy(info->kh_memory, &kh, sizeof(kh));
	set_nr_cpus(dh.nr_cpus);

	if (dh.header_version >= 3) {
		/* A dumpfile contains vmcoreinfo data. */
		set_vmcoreinfo(kh.offset_vmcoreinfo, kh.size_vmcoreinfo);
	}
	if (dh.header_version >= 4) {
		/* A dumpfile contains ELF note section. */
		set_pt_note(kh.offset_note, kh.size_note);
	}
	if (dh.header_version >= 5) {
		/* A dumpfile contains erased information. */
		set_eraseinfo(kh.offset_eraseinfo, kh.size_eraseinfo);
	}
	return TRUE;
error:
	free(info->dh_memory);
	info->dh_memory = NULL;

	return FALSE;
}

int
open_dump_memory(void)
{
	int fd, status;

	if ((fd = open(info->name_memory, O_RDONLY)) < 0) {
		ERRMSG("Can't open the dump memory(%s). %s\n",
		    info->name_memory, strerror(errno));
		return FALSE;
	}
	info->fd_memory = fd;

	status = check_kdump_compressed(info->name_memory);
	if (status == TRUE) {
		info->flag_refiltering = TRUE;
		return get_kdump_compressed_header_info(info->name_memory);
	} else if (status == FALSE) {
		return TRUE;
	} else {
		return FALSE;
	}
}

int
open_dump_file(void)
{
	int fd;
	int open_flags = O_RDWR|O_CREAT|O_TRUNC;

	if (!info->flag_force)
		open_flags |= O_EXCL;

	if (info->flag_flatten) {
		fd = STDOUT_FILENO;
		info->name_dumpfile = filename_stdout;
	} else if ((fd = open(info->name_dumpfile, open_flags,
	    S_IRUSR|S_IWUSR)) < 0) {
		ERRMSG("Can't open the dump file(%s). %s\n",
		    info->name_dumpfile, strerror(errno));
		return FALSE;
	}
	info->fd_dumpfile = fd;
	return TRUE;
}

int
open_dump_bitmap(void)
{
	int i, fd;
	char *tmpname;

	tmpname = getenv("TMPDIR");
	if (!tmpname)
		tmpname = "/tmp";

	if ((info->name_bitmap = (char *)malloc(sizeof(FILENAME_BITMAP) +
						strlen(tmpname) + 1)) == NULL) {
		ERRMSG("Can't allocate memory for the filename. %s\n",
		    strerror(errno));
		return FALSE;
	}
	strcpy(info->name_bitmap, tmpname);
	strcat(info->name_bitmap, "/");
	strcat(info->name_bitmap, FILENAME_BITMAP);
	if ((fd = mkstemp(info->name_bitmap)) < 0) {
		ERRMSG("Can't open the bitmap file(%s). %s\n",
		    info->name_bitmap, strerror(errno));
		return FALSE;
	}
	info->fd_bitmap = fd;

	if (info->flag_split) {
		/*
		 * Reserve file descriptors of bitmap for creating split
		 * dumpfiles by multiple processes, because a bitmap file will
		 * be unlinked just after this and it is not possible to open
		 * a bitmap file later.
		 */
		for (i = 0; i < info->num_dumpfile; i++) {
			if ((fd = open(info->name_bitmap, O_RDONLY)) < 0) {
				ERRMSG("Can't open the bitmap file(%s). %s\n",
				    info->name_bitmap, strerror(errno));
				return FALSE;
			}
			SPLITTING_FD_BITMAP(i) = fd;
		}
	}
	unlink(info->name_bitmap);

	return TRUE;
}

/*
 * Open the following files when it generates the vmcoreinfo file.
 * - vmlinux
 * - vmcoreinfo file
 */
int
open_files_for_generating_vmcoreinfo(void)
{
	if (!open_kernel_file())
		return FALSE;

	if (!open_vmcoreinfo("w"))
		return FALSE;

	return TRUE;
}

/*
 * Open the following file when it rearranges the dump data.
 * - dump file
 */
int
open_files_for_rearranging_dumpdata(void)
{
	if (!open_dump_file())
		return FALSE;

	return TRUE;
}

/*
 * Open the following files when it creates the dump file.
 * - dump mem
 * - dump file
 * - bit map
 * if it reads the vmcoreinfo file
 *   - vmcoreinfo file
 * else
 *   - vmlinux
 */
int
open_files_for_creating_dumpfile(void)
{
	if (info->flag_read_vmcoreinfo) {
		if (!open_vmcoreinfo("r"))
			return FALSE;
	} else {
		if (!open_kernel_file())
			return FALSE;
	}
	if (!open_dump_memory())
		return FALSE;

	if (!open_dump_bitmap())
		return FALSE;

	return TRUE;
}

static struct module_info *
get_loaded_module(char *mod_name)
{
	unsigned int i;
	struct module_info *modules;

	modules = mod_st.modules;
	if (strcmp(mod_name, modules[mod_st.current_mod].name)) {
		for (i = 0; i < mod_st.num_modules; i++) {
			if (!strcmp(mod_name, modules[i].name))
				break;
		}
		if (i == mod_st.num_modules)
			return NULL;
		/* set the current_mod for fast lookup next time */
		mod_st.current_mod = i;
	}

	return &modules[mod_st.current_mod];
}

int
sym_in_module(char *symname, unsigned long long *symbol_addr)
{
	int i;
	char *module_name;
	struct module_info *module_ptr;
	struct symbol_info *sym_info;

	module_name = get_dwarf_module_name();
	if (!mod_st.num_modules
		|| !strcmp(module_name, "vmlinux")
		|| !strcmp(module_name, "xen-syms"))
		return FALSE;

	module_ptr = get_loaded_module(module_name);
	if (!module_ptr)
		return FALSE;
	sym_info = module_ptr->sym_info;
	if (!sym_info)
		return FALSE;
	for (i = 1; i < module_ptr->num_syms; i++) {
		if (sym_info[i].name && !strcmp(sym_info[i].name, symname)) {
			*symbol_addr = sym_info[i].value;
			return TRUE;
		}
	}
	return FALSE;
}

int
is_kvaddr(unsigned long long addr)
{
	return (addr >= (unsigned long long)(KVBASE));
}

int
get_symbol_info(void)
{
	/*
	 * Get symbol info.
	 */
	SYMBOL_INIT(mem_map, "mem_map");
	SYMBOL_INIT(vmem_map, "vmem_map");
	SYMBOL_INIT(mem_section, "mem_section");
	SYMBOL_INIT(pkmap_count, "pkmap_count");
	SYMBOL_INIT_NEXT(pkmap_count_next, "pkmap_count");
	SYMBOL_INIT(system_utsname, "system_utsname");
	SYMBOL_INIT(init_uts_ns, "init_uts_ns");
	SYMBOL_INIT(_stext, "_stext");
	SYMBOL_INIT(swapper_pg_dir, "swapper_pg_dir");
	SYMBOL_INIT(init_level4_pgt, "init_level4_pgt");
	SYMBOL_INIT(vmlist, "vmlist");
	SYMBOL_INIT(phys_base, "phys_base");
	SYMBOL_INIT(node_online_map, "node_online_map");
	SYMBOL_INIT(node_states, "node_states");
	SYMBOL_INIT(node_memblk, "node_memblk");
	SYMBOL_INIT(node_data, "node_data");
	SYMBOL_INIT(pgdat_list, "pgdat_list");
	SYMBOL_INIT(contig_page_data, "contig_page_data");
	SYMBOL_INIT(log_buf, "log_buf");
	SYMBOL_INIT(log_buf_len, "log_buf_len");
	SYMBOL_INIT(log_end, "log_end");
	SYMBOL_INIT(max_pfn, "max_pfn");
	SYMBOL_INIT(modules, "modules");

	if (SYMBOL(node_data) != NOT_FOUND_SYMBOL)
		SYMBOL_ARRAY_TYPE_INIT(node_data, "node_data");
	if (SYMBOL(pgdat_list) != NOT_FOUND_SYMBOL)
		SYMBOL_ARRAY_LENGTH_INIT(pgdat_list, "pgdat_list");
	if (SYMBOL(mem_section) != NOT_FOUND_SYMBOL)
		SYMBOL_ARRAY_LENGTH_INIT(mem_section, "mem_section");
	if (SYMBOL(node_memblk) != NOT_FOUND_SYMBOL)
		SYMBOL_ARRAY_LENGTH_INIT(node_memblk, "node_memblk");

	return TRUE;
}

int
get_structure_info(void)
{
	/*
	 * Get offsets of the page_discriptor's members.
	 */
	SIZE_INIT(page, "page");
	OFFSET_INIT(page.flags, "page", "flags");
	OFFSET_INIT(page._count, "page", "_count");

	OFFSET_INIT(page.mapping, "page", "mapping");

	/*
	 * On linux-2.6.16 or later, page.mapping is defined
	 * in anonymous union.
	 */
	if (OFFSET(page.mapping) == NOT_FOUND_STRUCTURE)
		OFFSET_IN_UNION_INIT(page.mapping, "page", "mapping");

	/*
	 * Some vmlinux(s) don't have debugging information about
	 * page.mapping. Then, makedumpfile assumes that there is
	 * "mapping" next to "private(unsigned long)" in the first
	 * union.
	 */
	if (OFFSET(page.mapping) == NOT_FOUND_STRUCTURE) {
		OFFSET(page.mapping) = get_member_offset("page", NULL,
		    DWARF_INFO_GET_MEMBER_OFFSET_1ST_UNION);
		if (OFFSET(page.mapping) == FAILED_DWARFINFO)
			return FALSE;
		if (OFFSET(page.mapping) != NOT_FOUND_STRUCTURE)
			OFFSET(page.mapping) += sizeof(unsigned long);
	}

	OFFSET_INIT(page.lru, "page", "lru");

	/*
	 * Get offsets of the mem_section's members.
	 */
	SIZE_INIT(mem_section, "mem_section");
	OFFSET_INIT(mem_section.section_mem_map, "mem_section",
	    "section_mem_map");

	/*
	 * Get offsets of the pglist_data's members.
	 */
	SIZE_INIT(pglist_data, "pglist_data");
	OFFSET_INIT(pglist_data.node_zones, "pglist_data", "node_zones");
	OFFSET_INIT(pglist_data.nr_zones, "pglist_data", "nr_zones");
	OFFSET_INIT(pglist_data.node_mem_map, "pglist_data", "node_mem_map");
	OFFSET_INIT(pglist_data.node_start_pfn, "pglist_data","node_start_pfn");
	OFFSET_INIT(pglist_data.node_spanned_pages, "pglist_data",
	    "node_spanned_pages");
	OFFSET_INIT(pglist_data.pgdat_next, "pglist_data", "pgdat_next");

	/*
	 * Get offsets of the zone's members.
	 */
	SIZE_INIT(zone, "zone");
	OFFSET_INIT(zone.free_pages, "zone", "free_pages");
	OFFSET_INIT(zone.free_area, "zone", "free_area");
	OFFSET_INIT(zone.vm_stat, "zone", "vm_stat");
	OFFSET_INIT(zone.spanned_pages, "zone", "spanned_pages");
	MEMBER_ARRAY_LENGTH_INIT(zone.free_area, "zone", "free_area");

	/*
	 * Get offsets of the free_area's members.
	 */
	SIZE_INIT(free_area, "free_area");
	OFFSET_INIT(free_area.free_list, "free_area", "free_list");
	MEMBER_ARRAY_LENGTH_INIT(free_area.free_list, "free_area", "free_list");

	/*
	 * Get offsets of the list_head's members.
	 */
	SIZE_INIT(list_head, "list_head");
	OFFSET_INIT(list_head.next, "list_head", "next");
	OFFSET_INIT(list_head.prev, "list_head", "prev");

	/*
	 * Get offsets of the node_memblk_s's members.
	 */
	SIZE_INIT(node_memblk_s, "node_memblk_s");
	OFFSET_INIT(node_memblk_s.start_paddr, "node_memblk_s", "start_paddr");
	OFFSET_INIT(node_memblk_s.size, "node_memblk_s", "size");
	OFFSET_INIT(node_memblk_s.nid, "node_memblk_s", "nid");

	OFFSET_INIT(vm_struct.addr, "vm_struct", "addr");

	/*
	 * Get offset of the module members.
	 */
	SIZE_INIT(module, "module");
	OFFSET_INIT(module.strtab, "module", "strtab");
	OFFSET_INIT(module.symtab, "module", "symtab");
	OFFSET_INIT(module.num_symtab, "module", "num_symtab");
	OFFSET_INIT(module.list, "module", "list");
	OFFSET_INIT(module.name, "module", "name");
	OFFSET_INIT(module.module_core, "module", "module_core");
	OFFSET_INIT(module.core_size, "module", "core_size");
	OFFSET_INIT(module.module_init, "module", "module_init");
	OFFSET_INIT(module.init_size, "module", "init_size");

	ENUM_NUMBER_INIT(NR_FREE_PAGES, "NR_FREE_PAGES");
	ENUM_NUMBER_INIT(N_ONLINE, "N_ONLINE");

	ENUM_NUMBER_INIT(PG_lru, "PG_lru");
	ENUM_NUMBER_INIT(PG_private, "PG_private");
	ENUM_NUMBER_INIT(PG_swapcache, "PG_swapcache");

	TYPEDEF_SIZE_INIT(nodemask_t, "nodemask_t");

	return TRUE;
}

int
get_srcfile_info(void)
{
	TYPEDEF_SRCFILE_INIT(pud_t, "pud_t");

	return TRUE;
}

int
get_value_for_old_linux(void)
{
	if (NUMBER(PG_lru) == NOT_FOUND_NUMBER)
		NUMBER(PG_lru) = PG_lru_ORIGINAL;
	if (NUMBER(PG_private) == NOT_FOUND_NUMBER)
		NUMBER(PG_private) = PG_private_ORIGINAL;
	if (NUMBER(PG_swapcache) == NOT_FOUND_NUMBER)
		NUMBER(PG_swapcache) = PG_swapcache_ORIGINAL;
	return TRUE;
}

int
get_str_osrelease_from_vmlinux(void)
{
	int fd;
	char *name;
	struct utsname system_utsname;
	unsigned long long utsname;
	off_t offset;
	const off_t failed = (off_t)-1;

	/*
	 * Get the kernel version.
	 */
	if (SYMBOL(system_utsname) != NOT_FOUND_SYMBOL) {
		utsname = SYMBOL(system_utsname);
	} else if (SYMBOL(init_uts_ns) != NOT_FOUND_SYMBOL) {
		utsname = SYMBOL(init_uts_ns) + sizeof(int);
	} else {
		ERRMSG("Can't get the symbol of system_utsname.\n");
		return FALSE;
	}
	get_fileinfo_of_debuginfo(&fd, &name);

	offset = vaddr_to_offset_slow(fd, name, utsname);
	if (!offset) {
		ERRMSG("Can't convert vaddr (%llx) of utsname to an offset.\n",
		    utsname);
		return FALSE;
	}
	if (lseek(fd, offset, SEEK_SET) == failed) {
		ERRMSG("Can't seek %s. %s\n", name, strerror(errno));
		return FALSE;
	}
	if (read(fd, &system_utsname, sizeof system_utsname)
	    != sizeof system_utsname) {
		ERRMSG("Can't read %s. %s\n", name, strerror(errno));
		return FALSE;
	}
	if (!strncpy(info->release, system_utsname.release, STRLEN_OSRELEASE)){
		ERRMSG("Can't do strncpy for osrelease.");
		return FALSE;
	}
	return TRUE;
}

int
is_sparsemem_extreme(void)
{
	if (ARRAY_LENGTH(mem_section)
	     == (NR_MEM_SECTIONS() / _SECTIONS_PER_ROOT_EXTREME()))
		return TRUE;
	else
		return FALSE;
}

int
get_mem_type(void)
{
	int ret;

	if ((SIZE(page) == NOT_FOUND_STRUCTURE)
	    || (OFFSET(page.flags) == NOT_FOUND_STRUCTURE)
	    || (OFFSET(page._count) == NOT_FOUND_STRUCTURE)
	    || (OFFSET(page.mapping) == NOT_FOUND_STRUCTURE)) {
		ret = NOT_FOUND_MEMTYPE;
	} else if ((((SYMBOL(node_data) != NOT_FOUND_SYMBOL)
	        && (ARRAY_LENGTH(node_data) != NOT_FOUND_STRUCTURE))
	    || ((SYMBOL(pgdat_list) != NOT_FOUND_SYMBOL)
	        && (OFFSET(pglist_data.pgdat_next) != NOT_FOUND_STRUCTURE))
	    || ((SYMBOL(pgdat_list) != NOT_FOUND_SYMBOL)
	        && (ARRAY_LENGTH(pgdat_list) != NOT_FOUND_STRUCTURE)))
	    && (SIZE(pglist_data) != NOT_FOUND_STRUCTURE)
	    && (OFFSET(pglist_data.node_mem_map) != NOT_FOUND_STRUCTURE)
	    && (OFFSET(pglist_data.node_start_pfn) != NOT_FOUND_STRUCTURE)
	    && (OFFSET(pglist_data.node_spanned_pages) !=NOT_FOUND_STRUCTURE)){
		ret = DISCONTIGMEM;
	} else if ((SYMBOL(mem_section) != NOT_FOUND_SYMBOL)
	    && (SIZE(mem_section) != NOT_FOUND_STRUCTURE)
	    && (OFFSET(mem_section.section_mem_map) != NOT_FOUND_STRUCTURE)
	    && (ARRAY_LENGTH(mem_section) != NOT_FOUND_STRUCTURE)) {
		if (is_sparsemem_extreme())
			ret = SPARSEMEM_EX;
		else
			ret = SPARSEMEM;
	} else if (SYMBOL(mem_map) != NOT_FOUND_SYMBOL) {
		ret = FLATMEM;
	} else {
		ret = NOT_FOUND_MEMTYPE;
	}

	return ret;
}

int
generate_vmcoreinfo(void)
{
	if (!set_page_size(sysconf(_SC_PAGE_SIZE)))
		return FALSE;

	set_dwarf_debuginfo("vmlinux", NULL,
			    info->name_vmlinux, info->fd_vmlinux);

	if (!get_symbol_info())
		return FALSE;

	if (!get_structure_info())
		return FALSE;

	if (!get_srcfile_info())
		return FALSE;

	if ((SYMBOL(system_utsname) == NOT_FOUND_SYMBOL)
	    && (SYMBOL(init_uts_ns) == NOT_FOUND_SYMBOL)) {
		ERRMSG("Can't get the symbol of system_utsname.\n");
		return FALSE;
	}
	if (!get_str_osrelease_from_vmlinux())
		return FALSE;

	if (!(info->kernel_version = get_kernel_version(info->release)))
		return FALSE;

	if (get_mem_type() == NOT_FOUND_MEMTYPE) {
		ERRMSG("Can't find the memory type.\n");
		return FALSE;
	}

	/*
	 * write 1st kernel's OSRELEASE
	 */
	fprintf(info->file_vmcoreinfo, "%s%s\n", STR_OSRELEASE,
	    info->release);

	/*
	 * write 1st kernel's PAGESIZE
	 */
	fprintf(info->file_vmcoreinfo, "%s%ld\n", STR_PAGESIZE,
	    info->page_size);

	/*
	 * write the symbol of 1st kernel
	 */
	WRITE_SYMBOL("mem_map", mem_map);
	WRITE_SYMBOL("vmem_map", vmem_map);
	WRITE_SYMBOL("mem_section", mem_section);
	WRITE_SYMBOL("pkmap_count", pkmap_count);
	WRITE_SYMBOL("pkmap_count_next", pkmap_count_next);
	WRITE_SYMBOL("system_utsname", system_utsname);
	WRITE_SYMBOL("init_uts_ns", init_uts_ns);
	WRITE_SYMBOL("_stext", _stext);
	WRITE_SYMBOL("swapper_pg_dir", swapper_pg_dir);
	WRITE_SYMBOL("init_level4_pgt", init_level4_pgt);
	WRITE_SYMBOL("vmlist", vmlist);
	WRITE_SYMBOL("phys_base", phys_base);
	WRITE_SYMBOL("node_online_map", node_online_map);
	WRITE_SYMBOL("node_states", node_states);
	WRITE_SYMBOL("node_data", node_data);
	WRITE_SYMBOL("pgdat_list", pgdat_list);
	WRITE_SYMBOL("contig_page_data", contig_page_data);
	WRITE_SYMBOL("log_buf", log_buf);
	WRITE_SYMBOL("log_buf_len", log_buf_len);
	WRITE_SYMBOL("log_end", log_end);
	WRITE_SYMBOL("max_pfn", max_pfn);

	/*
	 * write the structure size of 1st kernel
	 */
	WRITE_STRUCTURE_SIZE("page", page);
	WRITE_STRUCTURE_SIZE("mem_section", mem_section);
	WRITE_STRUCTURE_SIZE("pglist_data", pglist_data);
	WRITE_STRUCTURE_SIZE("zone", zone);
	WRITE_STRUCTURE_SIZE("free_area", free_area);
	WRITE_STRUCTURE_SIZE("list_head", list_head);
	WRITE_STRUCTURE_SIZE("node_memblk_s", node_memblk_s);
	WRITE_STRUCTURE_SIZE("nodemask_t", nodemask_t);

	/*
	 * write the member offset of 1st kernel
	 */
	WRITE_MEMBER_OFFSET("page.flags", page.flags);
	WRITE_MEMBER_OFFSET("page._count", page._count);
	WRITE_MEMBER_OFFSET("page.mapping", page.mapping);
	WRITE_MEMBER_OFFSET("page.lru", page.lru);
	WRITE_MEMBER_OFFSET("mem_section.section_mem_map",
	    mem_section.section_mem_map);
	WRITE_MEMBER_OFFSET("pglist_data.node_zones", pglist_data.node_zones);
	WRITE_MEMBER_OFFSET("pglist_data.nr_zones", pglist_data.nr_zones);
	WRITE_MEMBER_OFFSET("pglist_data.node_mem_map",
	    pglist_data.node_mem_map);
	WRITE_MEMBER_OFFSET("pglist_data.node_start_pfn",
	    pglist_data.node_start_pfn);
	WRITE_MEMBER_OFFSET("pglist_data.node_spanned_pages",
	    pglist_data.node_spanned_pages);
	WRITE_MEMBER_OFFSET("pglist_data.pgdat_next", pglist_data.pgdat_next);
	WRITE_MEMBER_OFFSET("zone.free_pages", zone.free_pages);
	WRITE_MEMBER_OFFSET("zone.free_area", zone.free_area);
	WRITE_MEMBER_OFFSET("zone.vm_stat", zone.vm_stat);
	WRITE_MEMBER_OFFSET("zone.spanned_pages", zone.spanned_pages);
	WRITE_MEMBER_OFFSET("free_area.free_list", free_area.free_list);
	WRITE_MEMBER_OFFSET("list_head.next", list_head.next);
	WRITE_MEMBER_OFFSET("list_head.prev", list_head.prev);
	WRITE_MEMBER_OFFSET("node_memblk_s.start_paddr", node_memblk_s.start_paddr);
	WRITE_MEMBER_OFFSET("node_memblk_s.size", node_memblk_s.size);
	WRITE_MEMBER_OFFSET("node_memblk_s.nid", node_memblk_s.nid);
	WRITE_MEMBER_OFFSET("vm_struct.addr", vm_struct.addr);

	if (SYMBOL(node_data) != NOT_FOUND_SYMBOL)
		WRITE_ARRAY_LENGTH("node_data", node_data);
	if (SYMBOL(pgdat_list) != NOT_FOUND_SYMBOL)
		WRITE_ARRAY_LENGTH("pgdat_list", pgdat_list);
	if (SYMBOL(mem_section) != NOT_FOUND_SYMBOL)
		WRITE_ARRAY_LENGTH("mem_section", mem_section);
	if (SYMBOL(node_memblk) != NOT_FOUND_SYMBOL)
		WRITE_ARRAY_LENGTH("node_memblk", node_memblk);

	WRITE_ARRAY_LENGTH("zone.free_area", zone.free_area);
	WRITE_ARRAY_LENGTH("free_area.free_list", free_area.free_list);

	WRITE_NUMBER("NR_FREE_PAGES", NR_FREE_PAGES);
	WRITE_NUMBER("N_ONLINE", N_ONLINE);

	WRITE_NUMBER("PG_lru", PG_lru);
	WRITE_NUMBER("PG_private", PG_private);
	WRITE_NUMBER("PG_swapcache", PG_swapcache);

	/*
	 * write the source file of 1st kernel
	 */
	WRITE_SRCFILE("pud_t", pud_t);

	return TRUE;
}

int
read_vmcoreinfo_basic_info(void)
{
	time_t tv_sec = 0;
	long page_size = FALSE;
	char buf[BUFSIZE_FGETS], *endp;
	unsigned int get_release = FALSE, i;

	if (fseek(info->file_vmcoreinfo, 0, SEEK_SET) < 0) {
		ERRMSG("Can't seek the vmcoreinfo file(%s). %s\n",
		    info->name_vmcoreinfo, strerror(errno));
		return FALSE;
	}

	while (fgets(buf, BUFSIZE_FGETS, info->file_vmcoreinfo)) {
		i = strlen(buf);
		if (!i)
			break;
		if (buf[i - 1] == '\n')
			buf[i - 1] = '\0';
		if (strncmp(buf, STR_OSRELEASE, strlen(STR_OSRELEASE)) == 0) {
			get_release = TRUE;
			/* if the release have been stored, skip this time. */
			if (strlen(info->release))
				continue;
			strcpy(info->release, buf + strlen(STR_OSRELEASE));
		}
		if (strncmp(buf, STR_PAGESIZE, strlen(STR_PAGESIZE)) == 0) {
			page_size = strtol(buf+strlen(STR_PAGESIZE),&endp,10);
			if ((!page_size || page_size == LONG_MAX)
			    || strlen(endp) != 0) {
				ERRMSG("Invalid data in %s: %s",
				    info->name_vmcoreinfo, buf);
				return FALSE;
			}
			if (!set_page_size(page_size)) {
				ERRMSG("Invalid data in %s: %s",
				    info->name_vmcoreinfo, buf);
				return FALSE;
			}
		}
		if (strncmp(buf, STR_CRASHTIME, strlen(STR_CRASHTIME)) == 0) {
			tv_sec = strtol(buf+strlen(STR_CRASHTIME),&endp,10);
			if ((!tv_sec || tv_sec == LONG_MAX)
			    || strlen(endp) != 0) {
				ERRMSG("Invalid data in %s: %s",
				    info->name_vmcoreinfo, buf);
				return FALSE;
			}
			info->timestamp.tv_sec = tv_sec;
		}
		if (strncmp(buf, STR_CONFIG_X86_PAE,
		    strlen(STR_CONFIG_X86_PAE)) == 0)
			vt.mem_flags |= MEMORY_X86_PAE;

		if (strncmp(buf, STR_CONFIG_PGTABLE_3,
		    strlen(STR_CONFIG_PGTABLE_3)) == 0)
			vt.mem_flags |= MEMORY_PAGETABLE_3L;

		if (strncmp(buf, STR_CONFIG_PGTABLE_4,
		    strlen(STR_CONFIG_PGTABLE_4)) == 0)
			vt.mem_flags |= MEMORY_PAGETABLE_4L;
	}
	if (!get_release || !info->page_size) {
		ERRMSG("Invalid format in %s", info->name_vmcoreinfo);
		return FALSE;
	}
	return TRUE;
}

unsigned long
read_vmcoreinfo_symbol(char *str_symbol)
{
	unsigned long symbol = NOT_FOUND_SYMBOL;
	char buf[BUFSIZE_FGETS], *endp;
	unsigned int i;

	if (fseek(info->file_vmcoreinfo, 0, SEEK_SET) < 0) {
		ERRMSG("Can't seek the vmcoreinfo file(%s). %s\n",
		    info->name_vmcoreinfo, strerror(errno));
		return INVALID_SYMBOL_DATA;
	}

	while (fgets(buf, BUFSIZE_FGETS, info->file_vmcoreinfo)) {
		i = strlen(buf);
		if (!i)
			break;
		if (buf[i - 1] == '\n')
			buf[i - 1] = '\0';
		if (strncmp(buf, str_symbol, strlen(str_symbol)) == 0) {
			symbol = strtoul(buf + strlen(str_symbol), &endp, 16);
			if ((!symbol || symbol == ULONG_MAX)
			    || strlen(endp) != 0) {
				ERRMSG("Invalid data in %s: %s",
				    info->name_vmcoreinfo, buf);
				return INVALID_SYMBOL_DATA;
			}
			break;
		}
	}
	return symbol;
}

long
read_vmcoreinfo_long(char *str_structure)
{
	long data = NOT_FOUND_LONG_VALUE;
	char buf[BUFSIZE_FGETS], *endp;
	unsigned int i;

	if (fseek(info->file_vmcoreinfo, 0, SEEK_SET) < 0) {
		ERRMSG("Can't seek the vmcoreinfo file(%s). %s\n",
		    info->name_vmcoreinfo, strerror(errno));
		return INVALID_STRUCTURE_DATA;
	}

	while (fgets(buf, BUFSIZE_FGETS, info->file_vmcoreinfo)) {
		i = strlen(buf);
		if (!i)
			break;
		if (buf[i - 1] == '\n')
			buf[i - 1] = '\0';
		if (strncmp(buf, str_structure, strlen(str_structure)) == 0) {
			data = strtol(buf + strlen(str_structure), &endp, 10);
			if ((data == LONG_MAX) || strlen(endp) != 0) {
				ERRMSG("Invalid data in %s: %s",
				    info->name_vmcoreinfo, buf);
				return INVALID_STRUCTURE_DATA;
			}
			break;
		}
	}
	return data;
}

int
read_vmcoreinfo_string(char *str_in, char *str_out)
{
	char buf[BUFSIZE_FGETS];
	unsigned int i;

	if (fseek(info->file_vmcoreinfo, 0, SEEK_SET) < 0) {
		ERRMSG("Can't seek the vmcoreinfo file(%s). %s\n",
		    info->name_vmcoreinfo, strerror(errno));
		return FALSE;
	}

	while (fgets(buf, BUFSIZE_FGETS, info->file_vmcoreinfo)) {
		i = strlen(buf);
		if (!i)
			break;
		if (buf[i - 1] == '\n')
			buf[i - 1] = '\0';
		if (strncmp(buf, str_in, strlen(str_in)) == 0) {
			strncpy(str_out, buf + strlen(str_in), LEN_SRCFILE - strlen(str_in));
			break;
		}
	}
	return TRUE;
}

int
read_vmcoreinfo(void)
{
	if (!read_vmcoreinfo_basic_info())
		return FALSE;

	READ_SYMBOL("mem_map", mem_map);
	READ_SYMBOL("vmem_map", vmem_map);
	READ_SYMBOL("mem_section", mem_section);
	READ_SYMBOL("pkmap_count", pkmap_count);
	READ_SYMBOL("pkmap_count_next", pkmap_count_next);
	READ_SYMBOL("system_utsname", system_utsname);
	READ_SYMBOL("init_uts_ns", init_uts_ns);
	READ_SYMBOL("_stext", _stext);
	READ_SYMBOL("swapper_pg_dir", swapper_pg_dir);
	READ_SYMBOL("init_level4_pgt", init_level4_pgt);
	READ_SYMBOL("vmlist", vmlist);
	READ_SYMBOL("phys_base", phys_base);
	READ_SYMBOL("node_online_map", node_online_map);
	READ_SYMBOL("node_states", node_states);
	READ_SYMBOL("node_data", node_data);
	READ_SYMBOL("pgdat_list", pgdat_list);
	READ_SYMBOL("contig_page_data", contig_page_data);
	READ_SYMBOL("log_buf", log_buf);
	READ_SYMBOL("log_buf_len", log_buf_len);
	READ_SYMBOL("log_end", log_end);
	READ_SYMBOL("max_pfn", max_pfn);

	READ_STRUCTURE_SIZE("page", page);
	READ_STRUCTURE_SIZE("mem_section", mem_section);
	READ_STRUCTURE_SIZE("pglist_data", pglist_data);
	READ_STRUCTURE_SIZE("zone", zone);
	READ_STRUCTURE_SIZE("free_area", free_area);
	READ_STRUCTURE_SIZE("list_head", list_head);
	READ_STRUCTURE_SIZE("node_memblk_s", node_memblk_s);
	READ_STRUCTURE_SIZE("nodemask_t", nodemask_t);

	READ_MEMBER_OFFSET("page.flags", page.flags);
	READ_MEMBER_OFFSET("page._count", page._count);
	READ_MEMBER_OFFSET("page.mapping", page.mapping);
	READ_MEMBER_OFFSET("page.lru", page.lru);
	READ_MEMBER_OFFSET("mem_section.section_mem_map",
	    mem_section.section_mem_map);
	READ_MEMBER_OFFSET("pglist_data.node_zones", pglist_data.node_zones);
	READ_MEMBER_OFFSET("pglist_data.nr_zones", pglist_data.nr_zones);
	READ_MEMBER_OFFSET("pglist_data.node_mem_map",pglist_data.node_mem_map);
	READ_MEMBER_OFFSET("pglist_data.node_start_pfn",
	    pglist_data.node_start_pfn);
	READ_MEMBER_OFFSET("pglist_data.node_spanned_pages",
	    pglist_data.node_spanned_pages);
	READ_MEMBER_OFFSET("pglist_data.pgdat_next", pglist_data.pgdat_next);
	READ_MEMBER_OFFSET("zone.free_pages", zone.free_pages);
	READ_MEMBER_OFFSET("zone.free_area", zone.free_area);
	READ_MEMBER_OFFSET("zone.vm_stat", zone.vm_stat);
	READ_MEMBER_OFFSET("zone.spanned_pages", zone.spanned_pages);
	READ_MEMBER_OFFSET("free_area.free_list", free_area.free_list);
	READ_MEMBER_OFFSET("list_head.next", list_head.next);
	READ_MEMBER_OFFSET("list_head.prev", list_head.prev);
	READ_MEMBER_OFFSET("node_memblk_s.start_paddr", node_memblk_s.start_paddr);
	READ_MEMBER_OFFSET("node_memblk_s.size", node_memblk_s.size);
	READ_MEMBER_OFFSET("node_memblk_s.nid", node_memblk_s.nid);
	READ_MEMBER_OFFSET("vm_struct.addr", vm_struct.addr);

	READ_ARRAY_LENGTH("node_data", node_data);
	READ_ARRAY_LENGTH("pgdat_list", pgdat_list);
	READ_ARRAY_LENGTH("mem_section", mem_section);
	READ_ARRAY_LENGTH("node_memblk", node_memblk);
	READ_ARRAY_LENGTH("zone.free_area", zone.free_area);
	READ_ARRAY_LENGTH("free_area.free_list", free_area.free_list);

	READ_NUMBER("NR_FREE_PAGES", NR_FREE_PAGES);
	READ_NUMBER("N_ONLINE", N_ONLINE);

	READ_NUMBER("PG_lru", PG_lru);
	READ_NUMBER("PG_private", PG_private);
	READ_NUMBER("PG_swapcache", PG_swapcache);

	READ_SRCFILE("pud_t", pud_t);

	return TRUE;
}

/*
 * Extract vmcoreinfo from /proc/vmcore and output it to /tmp/vmcoreinfo.tmp.
 */
int
copy_vmcoreinfo(off_t offset, unsigned long size)
{
	int fd;
	char buf[VMCOREINFO_BYTES];
	const off_t failed = (off_t)-1;

	if (!offset || !size)
		return FALSE;

	if ((fd = mkstemp(info->name_vmcoreinfo)) < 0) {
		ERRMSG("Can't open the vmcoreinfo file(%s). %s\n",
		    info->name_vmcoreinfo, strerror(errno));
		return FALSE;
	}
	if (lseek(info->fd_memory, offset, SEEK_SET) == failed) {
		ERRMSG("Can't seek the dump memory(%s). %s\n",
		    info->name_memory, strerror(errno));
		return FALSE;
	}
	if (read(info->fd_memory, &buf, size) != size) {
		ERRMSG("Can't read the dump memory(%s). %s\n",
		    info->name_memory, strerror(errno));
		return FALSE;
	}
	if (write(fd, &buf, size) != size) {
		ERRMSG("Can't write the vmcoreinfo file(%s). %s\n",
		    info->name_vmcoreinfo, strerror(errno));
		return FALSE;
	}
	if (close(fd) < 0) {
		ERRMSG("Can't close the vmcoreinfo file(%s). %s\n",
		    info->name_vmcoreinfo, strerror(errno));
		return FALSE;
	}
	return TRUE;
}

int
read_vmcoreinfo_from_vmcore(off_t offset, unsigned long size, int flag_xen_hv)
{
	int ret = FALSE;

	/*
	 * Copy vmcoreinfo to /tmp/vmcoreinfoXXXXXX.
	 */
	if (!(info->name_vmcoreinfo = strdup(FILENAME_VMCOREINFO))) {
		MSG("Can't duplicate strings(%s).\n", FILENAME_VMCOREINFO);
		return FALSE;
	}
	if (!copy_vmcoreinfo(offset, size))
		goto out;

	/*
	 * Read vmcoreinfo from /tmp/vmcoreinfoXXXXXX.
	 */
	if (!open_vmcoreinfo("r"))
		goto out;

	unlink(info->name_vmcoreinfo);

	if (flag_xen_hv) {
		if (!read_vmcoreinfo_xen())
			goto out;
	} else {
		if (!read_vmcoreinfo())
			goto out;
	}
	close_vmcoreinfo();

	ret = TRUE;
out:
	free(info->name_vmcoreinfo);
	info->name_vmcoreinfo = NULL;

	return ret;
}

/*
 * Get the number of online nodes.
 */
int
get_nodes_online(void)
{
	int len, i, j, online;
	unsigned long node_online_map = 0, bitbuf, *maskptr;

	if ((SYMBOL(node_online_map) == NOT_FOUND_SYMBOL)
	    && (SYMBOL(node_states) == NOT_FOUND_SYMBOL))
		return 0;

	if (SIZE(nodemask_t) == NOT_FOUND_STRUCTURE) {
		ERRMSG("Can't get the size of nodemask_t.\n");
		return 0;
	}

	len = SIZE(nodemask_t);
	vt.node_online_map_len = len/sizeof(unsigned long);
	if (!(vt.node_online_map = (unsigned long *)malloc(len))) {
		ERRMSG("Can't allocate memory for the node online map. %s\n",
		    strerror(errno));
		return 0;
	}
	if (SYMBOL(node_online_map) != NOT_FOUND_SYMBOL) {
		node_online_map = SYMBOL(node_online_map);
	} else if (SYMBOL(node_states) != NOT_FOUND_SYMBOL) {
		/*
		 * For linux-2.6.23-rc4-mm1
		 */
		node_online_map = SYMBOL(node_states)
		     + (SIZE(nodemask_t) * NUMBER(N_ONLINE));
	}
	if (!readmem(VADDR, node_online_map, vt.node_online_map, len)){
		ERRMSG("Can't get the node online map.\n");
		return 0;
	}
	online = 0;
	maskptr = (unsigned long *)vt.node_online_map;
	for (i = 0; i < vt.node_online_map_len; i++, maskptr++) {
		bitbuf = *maskptr;
		for (j = 0; j < sizeof(bitbuf) * 8; j++) {
			online += bitbuf & 1;
			bitbuf = bitbuf >> 1;
		}
	}
	return online;
}

int
get_numnodes(void)
{
	if (!(vt.numnodes = get_nodes_online())) {
		vt.numnodes = 1;
	}
	DEBUG_MSG("\n");
	DEBUG_MSG("num of NODEs : %d\n", vt.numnodes);
	DEBUG_MSG("\n");

	return TRUE;
}

int
next_online_node(int first)
{
	int i, j, node;
	unsigned long mask, *maskptr;

	/* It cannot occur */
	if ((first/(sizeof(unsigned long) * 8)) >= vt.node_online_map_len) {
		ERRMSG("next_online_node: %d is too large!\n", first);
		return -1;
	}

	maskptr = (unsigned long *)vt.node_online_map;
	for (i = node = 0; i <  vt.node_online_map_len; i++, maskptr++) {
		mask = *maskptr;
		for (j = 0; j < (sizeof(unsigned long) * 8); j++, node++) {
			if (mask & 1) {
				if (node >= first)
					return node;
			}
			mask >>= 1;
		}
	}
	return -1;
}

unsigned long
next_online_pgdat(int node)
{
	int i;
	unsigned long pgdat;

	/*
	 * Get the pglist_data structure from symbol "node_data".
	 *     The array number of symbol "node_data" cannot be gotten
	 *     from vmlinux. Instead, check it is DW_TAG_array_type.
	 */
	if ((SYMBOL(node_data) == NOT_FOUND_SYMBOL)
	    || (ARRAY_LENGTH(node_data) == NOT_FOUND_STRUCTURE))
		goto pgdat2;

	if (!readmem(VADDR, SYMBOL(node_data) + (node * sizeof(void *)),
	    &pgdat, sizeof pgdat))
		goto pgdat2;

	if (!is_kvaddr(pgdat))
		goto pgdat2;

	return pgdat;

pgdat2:
	/*
	 * Get the pglist_data structure from symbol "pgdat_list".
	 */
	if (SYMBOL(pgdat_list) == NOT_FOUND_SYMBOL)
		goto pgdat3;

	else if ((0 < node)
	    && (ARRAY_LENGTH(pgdat_list) == NOT_FOUND_STRUCTURE))
		goto pgdat3;

	else if ((ARRAY_LENGTH(pgdat_list) != NOT_FOUND_STRUCTURE)
	    && (ARRAY_LENGTH(pgdat_list) < node))
		goto pgdat3;

	if (!readmem(VADDR, SYMBOL(pgdat_list) + (node * sizeof(void *)),
	    &pgdat, sizeof pgdat))
		goto pgdat3;

	if (!is_kvaddr(pgdat))
		goto pgdat3;

	return pgdat;

pgdat3:
	/*
	 * linux-2.6.16 or former
	 */
	if ((SYMBOL(pgdat_list) == NOT_FOUND_SYMBOL)
	    || (OFFSET(pglist_data.pgdat_next) == NOT_FOUND_STRUCTURE))
		goto pgdat4;

	if (!readmem(VADDR, SYMBOL(pgdat_list), &pgdat, sizeof pgdat))
		goto pgdat4;

	if (!is_kvaddr(pgdat))
		goto pgdat4;

	if (node == 0)
		return pgdat;

	for (i = 1; i <= node; i++) {
		if (!readmem(VADDR, pgdat+OFFSET(pglist_data.pgdat_next),
		    &pgdat, sizeof pgdat))
			goto pgdat4;

		if (!is_kvaddr(pgdat))
			goto pgdat4;
	}
	return pgdat;

pgdat4:
	/*
	 * Get the pglist_data structure from symbol "contig_page_data".
	 */
	if (SYMBOL(contig_page_data) == NOT_FOUND_SYMBOL)
		return FALSE;

	if (node != 0)
		return FALSE;

	return SYMBOL(contig_page_data);
}

void
dump_mem_map(unsigned long long pfn_start,
    unsigned long long pfn_end, unsigned long mem_map, int num_mm)
{
	struct mem_map_data *mmd;

	mmd = &info->mem_map_data[num_mm];
	mmd->pfn_start = pfn_start;
	mmd->pfn_end   = pfn_end;
	mmd->mem_map   = mem_map;

	DEBUG_MSG("mem_map (%d)\n", num_mm);
	DEBUG_MSG("  mem_map    : %lx\n", mem_map);
	DEBUG_MSG("  pfn_start  : %llx\n", pfn_start);
	DEBUG_MSG("  pfn_end    : %llx\n", pfn_end);

	return;
}

int
get_mm_flatmem(void)
{
	unsigned long mem_map;

	/*
	 * Get the address of the symbol "mem_map".
	 */
	if (!readmem(VADDR, SYMBOL(mem_map), &mem_map, sizeof mem_map)
	    || !mem_map) {
		ERRMSG("Can't get the address of mem_map.\n");
		return FALSE;
	}
	info->num_mem_map = 1;
	if ((info->mem_map_data = (struct mem_map_data *)
	    malloc(sizeof(struct mem_map_data)*info->num_mem_map)) == NULL) {
		ERRMSG("Can't allocate memory for the mem_map_data. %s\n",
		    strerror(errno));
		return FALSE;
	}
	if (is_xen_memory())
		dump_mem_map(0, info->dom0_mapnr, mem_map, 0);
	else
		dump_mem_map(0, info->max_mapnr, mem_map, 0);

	return TRUE;
}

int
get_node_memblk(int num_memblk,
    unsigned long *start_paddr, unsigned long *size, int *nid)
{
	unsigned long node_memblk;

	if (ARRAY_LENGTH(node_memblk) <= num_memblk) {
		ERRMSG("Invalid num_memblk.\n");
		return FALSE;
	}
	node_memblk = SYMBOL(node_memblk) + SIZE(node_memblk_s) * num_memblk;
	if (!readmem(VADDR, node_memblk+OFFSET(node_memblk_s.start_paddr),
	    start_paddr, sizeof(unsigned long))) {
		ERRMSG("Can't get node_memblk_s.start_paddr.\n");
		return FALSE;
	}
	if (!readmem(VADDR, node_memblk + OFFSET(node_memblk_s.size),
	    size, sizeof(unsigned long))) {
		ERRMSG("Can't get node_memblk_s.size.\n");
		return FALSE;
	}
	if (!readmem(VADDR, node_memblk + OFFSET(node_memblk_s.nid),
	    nid, sizeof(int))) {
		ERRMSG("Can't get node_memblk_s.nid.\n");
		return FALSE;
	}
	return TRUE;
}

int
get_num_mm_discontigmem(void)
{
	int i, nid;
	unsigned long start_paddr, size;

	if ((SYMBOL(node_memblk) == NOT_FOUND_SYMBOL)
	    || (ARRAY_LENGTH(node_memblk) == NOT_FOUND_STRUCTURE)
	    || (SIZE(node_memblk_s) == NOT_FOUND_STRUCTURE)
	    || (OFFSET(node_memblk_s.start_paddr) == NOT_FOUND_STRUCTURE)
	    || (OFFSET(node_memblk_s.size) == NOT_FOUND_STRUCTURE)
	    || (OFFSET(node_memblk_s.nid) == NOT_FOUND_STRUCTURE)) {
		return vt.numnodes;
	} else {
		for (i = 0; i < ARRAY_LENGTH(node_memblk); i++) {
			if (!get_node_memblk(i, &start_paddr, &size, &nid)) {
				ERRMSG("Can't get the node_memblk (%d)\n", i);
				return 0;
			}
			if (!start_paddr && !size &&!nid)
				break;

			DEBUG_MSG("nid : %d\n", nid);
			DEBUG_MSG("  start_paddr: %lx\n", start_paddr);
			DEBUG_MSG("  size       : %lx\n", size);
		}
		if (i == 0) {
			/*
			 * On non-NUMA systems, node_memblk_s is not set.
			 */
			return vt.numnodes;
		} else {
			return i;
		}
	}
}

int
separate_mem_map(struct mem_map_data *mmd, int *id_mm, int nid_pgdat,
    unsigned long mem_map_pgdat, unsigned long pfn_start_pgdat)
{
	int i, nid;
	unsigned long start_paddr, size, pfn_start, pfn_end, mem_map;

	for (i = 0; i < ARRAY_LENGTH(node_memblk); i++) {
		if (!get_node_memblk(i, &start_paddr, &size, &nid)) {
			ERRMSG("Can't get the node_memblk (%d)\n", i);
			return FALSE;
		}
		if (!start_paddr && !size && !nid)
			break;

		/*
		 * Check pglist_data.node_id and node_memblk_s.nid match.
		 */
		if (nid_pgdat != nid)
			continue;

		pfn_start = paddr_to_pfn(start_paddr);
		pfn_end   = paddr_to_pfn(start_paddr + size);

		if (pfn_start < pfn_start_pgdat) {
			ERRMSG("node_memblk_s.start_paddr of node (%d) is invalid.\n", nid);
			return FALSE;
		}
		if (info->max_mapnr < pfn_end) {
			DEBUG_MSG("pfn_end of node (%d) is over max_mapnr.\n",
			    nid);
			DEBUG_MSG("  pfn_start: %lx\n", pfn_start);
			DEBUG_MSG("  pfn_end  : %lx\n", pfn_end);
			DEBUG_MSG("  max_mapnr: %llx\n", info->max_mapnr);

			pfn_end = info->max_mapnr;
		}

		mem_map = mem_map_pgdat+SIZE(page)*(pfn_start-pfn_start_pgdat);

		mmd->pfn_start = pfn_start;
		mmd->pfn_end   = pfn_end;
		mmd->mem_map   = mem_map;

		mmd++;
		(*id_mm)++;
	}
	return TRUE;
}

int
get_mm_discontigmem(void)
{
	int i, j, id_mm, node, num_mem_map, separate_mm = FALSE;
	unsigned long pgdat, mem_map, pfn_start, pfn_end, node_spanned_pages;
	unsigned long vmem_map;
	struct mem_map_data temp_mmd;

	num_mem_map = get_num_mm_discontigmem();
	if (num_mem_map < vt.numnodes) {
		ERRMSG("Can't get the number of mem_map.\n");
		return FALSE;
	}
	struct mem_map_data mmd[num_mem_map];
	if (vt.numnodes < num_mem_map) {
		separate_mm = TRUE;
	}

	/*
	 * Note:
	 *  This note is only for ia64 discontigmem kernel.
	 *  It is better to take mem_map information from a symbol vmem_map
	 *  instead of pglist_data.node_mem_map, because some node_mem_map
	 *  sometimes does not have mem_map information corresponding to its
	 *  node_start_pfn.
	 */
	if (SYMBOL(vmem_map) != NOT_FOUND_SYMBOL) {
		if (!readmem(VADDR, SYMBOL(vmem_map), &vmem_map, sizeof vmem_map)) {
			ERRMSG("Can't get vmem_map.\n");
			return FALSE;
		}
	}

	/*
	 * Get the first node_id.
	 */
	if ((node = next_online_node(0)) < 0) {
		ERRMSG("Can't get next online node.\n");
		return FALSE;
	}
	if (!(pgdat = next_online_pgdat(node))) {
		ERRMSG("Can't get pgdat list.\n");
		return FALSE;
	}
	id_mm = 0;
	for (i = 0; i < vt.numnodes; i++) {
		if (!readmem(VADDR, pgdat + OFFSET(pglist_data.node_start_pfn),
		    &pfn_start, sizeof pfn_start)) {
			ERRMSG("Can't get node_start_pfn.\n");
			return FALSE;
		}
		if (!readmem(VADDR,pgdat+OFFSET(pglist_data.node_spanned_pages),
		    &node_spanned_pages, sizeof node_spanned_pages)) {
			ERRMSG("Can't get node_spanned_pages.\n");
			return FALSE;
		}
		pfn_end = pfn_start + node_spanned_pages;

		if (SYMBOL(vmem_map) == NOT_FOUND_SYMBOL) {
			if (!readmem(VADDR, pgdat + OFFSET(pglist_data.node_mem_map),
			    &mem_map, sizeof mem_map)) {
				ERRMSG("Can't get mem_map.\n");
				return FALSE;
			}
		} else
			mem_map = vmem_map + (SIZE(page) * pfn_start);

		if (separate_mm) {
			/*
			 * For some ia64 NUMA systems.
			 * On some systems, a node has the separated memory.
			 * And pglist_data(s) have the duplicated memory range
			 * like following:
			 *
			 * Nid:      Physical address
			 *  0 : 0x1000000000 - 0x2000000000
			 *  1 : 0x2000000000 - 0x3000000000
			 *  2 : 0x0000000000 - 0x6020000000 <- Overlapping
			 *  3 : 0x3000000000 - 0x4000000000
			 *  4 : 0x4000000000 - 0x5000000000
			 *  5 : 0x5000000000 - 0x6000000000
			 *
			 * Then, mem_map(s) should be separated by
			 * node_memblk_s info.
			 */
			if (!separate_mem_map(&mmd[id_mm], &id_mm, node,
			    mem_map, pfn_start)) {
				ERRMSG("Can't separate mem_map.\n");
				return FALSE;
			}
		} else {
			if (info->max_mapnr < pfn_end) {
				DEBUG_MSG("pfn_end of node (%d) is over max_mapnr.\n",
				    node);
				DEBUG_MSG("  pfn_start: %lx\n", pfn_start);
				DEBUG_MSG("  pfn_end  : %lx\n", pfn_end);
				DEBUG_MSG("  max_mapnr: %llx\n", info->max_mapnr);

				pfn_end = info->max_mapnr;
			}

			/*
			 * The number of mem_map is the same as the number
			 * of nodes.
			 */
			mmd[id_mm].pfn_start = pfn_start;
			mmd[id_mm].pfn_end   = pfn_end;
			mmd[id_mm].mem_map   = mem_map;
			id_mm++;
		}

		/*
		 * Get pglist_data of the next node.
		 */
		if (i < (vt.numnodes - 1)) {
			if ((node = next_online_node(node + 1)) < 0) {
				ERRMSG("Can't get next online node.\n");
				return FALSE;
			} else if (!(pgdat = next_online_pgdat(node))) {
				ERRMSG("Can't determine pgdat list (node %d).\n",
				    node);
				return FALSE;
			}
		}
	}

	/*
	 * Sort mem_map by pfn_start.
	 */
	for (i = 0; i < (num_mem_map - 1); i++) {
		for (j = i + 1; j < num_mem_map; j++) {
			if (mmd[j].pfn_start < mmd[i].pfn_start) {
				temp_mmd = mmd[j];
				mmd[j] = mmd[i];
				mmd[i] = temp_mmd;
			}
		}
	}

	/*
	 * Calculate the number of mem_map.
	 */
	info->num_mem_map = num_mem_map;
	if (mmd[0].pfn_start != 0)
		info->num_mem_map++;

	for (i = 0; i < num_mem_map - 1; i++) {
		if (mmd[i].pfn_end > mmd[i + 1].pfn_start) {
			ERRMSG("The mem_map is overlapped with the next one.\n");
			ERRMSG("mmd[%d].pfn_end   = %llx\n", i, mmd[i].pfn_end);
			ERRMSG("mmd[%d].pfn_start = %llx\n", i + 1, mmd[i + 1].pfn_start);
			return FALSE;
		} else if (mmd[i].pfn_end == mmd[i + 1].pfn_start)
			/*
			 * Continuous mem_map
			 */
			continue;

		/*
		 * Discontinuous mem_map
		 */
		info->num_mem_map++;
	}
	if (mmd[num_mem_map - 1].pfn_end < info->max_mapnr)
		info->num_mem_map++;

	if ((info->mem_map_data = (struct mem_map_data *)
	    malloc(sizeof(struct mem_map_data)*info->num_mem_map)) == NULL) {
		ERRMSG("Can't allocate memory for the mem_map_data. %s\n",
		    strerror(errno));
		return FALSE;
	}

	/*
	 * Create mem_map data.
	 */
	id_mm = 0;
	if (mmd[0].pfn_start != 0) {
		dump_mem_map(0, mmd[0].pfn_start, NOT_MEMMAP_ADDR, id_mm);
		id_mm++;
	}
	for (i = 0; i < num_mem_map; i++) {
		dump_mem_map(mmd[i].pfn_start, mmd[i].pfn_end,
		    mmd[i].mem_map, id_mm);
		id_mm++;
		if ((i < num_mem_map - 1)
		    && (mmd[i].pfn_end != mmd[i + 1].pfn_start)) {
			dump_mem_map(mmd[i].pfn_end, mmd[i +1].pfn_start,
			    NOT_MEMMAP_ADDR, id_mm);
			id_mm++;
		}
	}
	i = num_mem_map - 1;
	if (is_xen_memory()) {
		if (mmd[i].pfn_end < info->dom0_mapnr)
			dump_mem_map(mmd[i].pfn_end, info->dom0_mapnr,
			    NOT_MEMMAP_ADDR, id_mm);
	} else {
		if (mmd[i].pfn_end < info->max_mapnr)
			dump_mem_map(mmd[i].pfn_end, info->max_mapnr,
			    NOT_MEMMAP_ADDR, id_mm);
	}
	return TRUE;
}

unsigned long
nr_to_section(unsigned long nr, unsigned long *mem_sec)
{
	unsigned long addr;

	if (is_sparsemem_extreme())
		addr = mem_sec[SECTION_NR_TO_ROOT(nr)] +
		    (nr & SECTION_ROOT_MASK()) * SIZE(mem_section);
	else
		addr = SYMBOL(mem_section) + (nr * SIZE(mem_section));

	if (!is_kvaddr(addr))
		return NOT_KV_ADDR;

	return addr;
}

unsigned long
section_mem_map_addr(unsigned long addr)
{
	char *mem_section;
	unsigned long map;

	if (!is_kvaddr(addr))
		return NOT_KV_ADDR;

	if ((mem_section = malloc(SIZE(mem_section))) == NULL) {
		ERRMSG("Can't allocate memory for a struct mem_section. %s\n",
		    strerror(errno));
		return NOT_KV_ADDR;
	}
	if (!readmem(VADDR, addr, mem_section, SIZE(mem_section))) {
		ERRMSG("Can't get a struct mem_section(%lx).\n", addr);
		free(mem_section);
		return NOT_KV_ADDR;
	}
	map = ULONG(mem_section + OFFSET(mem_section.section_mem_map));
	map &= SECTION_MAP_MASK;
	free(mem_section);

	return map;
}

unsigned long
sparse_decode_mem_map(unsigned long coded_mem_map, unsigned long section_nr)
{
	if (!is_kvaddr(coded_mem_map))
		return NOT_KV_ADDR;

	return coded_mem_map +
	    (SECTION_NR_TO_PFN(section_nr) * SIZE(page));
}

int
get_mm_sparsemem(void)
{
	unsigned int section_nr, mem_section_size, num_section;
	unsigned long long pfn_start, pfn_end;
	unsigned long section, mem_map;
	unsigned long *mem_sec = NULL;

	int ret = FALSE;

	/*
	 * Get the address of the symbol "mem_section".
	 */
	num_section = divideup(info->max_mapnr, PAGES_PER_SECTION());
	if (is_sparsemem_extreme()) {
		info->sections_per_root = _SECTIONS_PER_ROOT_EXTREME();
		mem_section_size = sizeof(void *) * NR_SECTION_ROOTS();
	} else {
		info->sections_per_root = _SECTIONS_PER_ROOT();
		mem_section_size = SIZE(mem_section) * NR_SECTION_ROOTS();
	}
	if ((mem_sec = malloc(mem_section_size)) == NULL) {
		ERRMSG("Can't allocate memory for the mem_section. %s\n",
		    strerror(errno));
		return FALSE;
	}
	if (!readmem(VADDR, SYMBOL(mem_section), mem_sec,
	    mem_section_size)) {
		ERRMSG("Can't get the address of mem_section.\n");
		goto out;
	}
	info->num_mem_map = num_section;
	if ((info->mem_map_data = (struct mem_map_data *)
	    malloc(sizeof(struct mem_map_data)*info->num_mem_map)) == NULL) {
		ERRMSG("Can't allocate memory for the mem_map_data. %s\n",
		    strerror(errno));
		goto out;
	}
	for (section_nr = 0; section_nr < num_section; section_nr++) {
		section = nr_to_section(section_nr, mem_sec);
		mem_map = section_mem_map_addr(section);
		mem_map = sparse_decode_mem_map(mem_map, section_nr);
		if (!is_kvaddr(mem_map))
			mem_map = NOT_MEMMAP_ADDR;
		pfn_start = section_nr * PAGES_PER_SECTION();
		pfn_end   = pfn_start + PAGES_PER_SECTION();
		if (info->max_mapnr < pfn_end)
			pfn_end = info->max_mapnr;
		dump_mem_map(pfn_start, pfn_end, mem_map, section_nr);
	}
	ret = TRUE;
out:
	if (mem_sec != NULL)
		free(mem_sec);

	return ret;
}

int
get_mem_map_without_mm(void)
{
	info->num_mem_map = 1;
	if ((info->mem_map_data = (struct mem_map_data *)
	    malloc(sizeof(struct mem_map_data)*info->num_mem_map)) == NULL) {
		ERRMSG("Can't allocate memory for the mem_map_data. %s\n",
		    strerror(errno));
		return FALSE;
	}
	if (is_xen_memory())
		dump_mem_map(0, info->dom0_mapnr, NOT_MEMMAP_ADDR, 0);
	else
		dump_mem_map(0, info->max_mapnr, NOT_MEMMAP_ADDR, 0);

	return TRUE;
}

int
get_mem_map(void)
{
	int ret;

	if (is_xen_memory()) {
		if (!get_dom0_mapnr()) {
			ERRMSG("Can't domain-0 pfn.\n");
			return FALSE;
		}
		DEBUG_MSG("domain-0 pfn : %llx\n", info->dom0_mapnr);
	}

	switch (get_mem_type()) {
	case SPARSEMEM:
		DEBUG_MSG("\n");
		DEBUG_MSG("Memory type  : SPARSEMEM\n");
		DEBUG_MSG("\n");
		ret = get_mm_sparsemem();
		break;
	case SPARSEMEM_EX:
		DEBUG_MSG("\n");
		DEBUG_MSG("Memory type  : SPARSEMEM_EX\n");
		DEBUG_MSG("\n");
		ret = get_mm_sparsemem();
		break;
	case DISCONTIGMEM:
		DEBUG_MSG("\n");
		DEBUG_MSG("Memory type  : DISCONTIGMEM\n");
		DEBUG_MSG("\n");
		ret = get_mm_discontigmem();
		break;
	case FLATMEM:
		DEBUG_MSG("\n");
		DEBUG_MSG("Memory type  : FLATMEM\n");
		DEBUG_MSG("\n");
		ret = get_mm_flatmem();
		break;
	default:
		ERRMSG("Can't distinguish the memory type.\n");
		ret = FALSE;
		break;
	}
	return ret;
}

int
initialize_bitmap_memory(void)
{
	struct disk_dump_header	*dh;
	struct dump_bitmap *bmp;
	off_t bitmap_offset;
	int bitmap_len, max_sect_len;
	unsigned long pfn;
	int i, j;
	long block_size;

	dh = info->dh_memory;
	block_size = dh->block_size;

	bitmap_offset
	    = (DISKDUMP_HEADER_BLOCKS + dh->sub_hdr_size) * block_size;
	bitmap_len = block_size * dh->bitmap_blocks;

	bmp = malloc(sizeof(struct dump_bitmap));
	if (bmp == NULL) {
		ERRMSG("Can't allocate memory for the memory-bitmap. %s\n",
		    strerror(errno));
		return FALSE;
	}
	bmp->fd        = info->fd_memory;
	bmp->file_name = info->name_memory;
	bmp->no_block  = -1;
	memset(bmp->buf, 0, BUFSIZE_BITMAP);
	bmp->offset = bitmap_offset + bitmap_len / 2;
	info->bitmap_memory = bmp;

	max_sect_len = divideup(dh->max_mapnr, BITMAP_SECT_LEN);
	info->valid_pages = calloc(sizeof(ulong), max_sect_len);
	if (info->valid_pages == NULL) {
		ERRMSG("Can't allocate memory for the valid_pages. %s\n",
		    strerror(errno));
		free(bmp);
		return FALSE;
	}
	for (i = 1, pfn = 0; i < max_sect_len; i++) {
		info->valid_pages[i] = info->valid_pages[i - 1];
		for (j = 0; j < BITMAP_SECT_LEN; j++, pfn++)
			if (is_dumpable(info->bitmap_memory, pfn))
				info->valid_pages[i]++;
	}

	return TRUE;
}

int
initial(void)
{
	off_t offset;
	unsigned long size;
	int debug_info = FALSE;

	if (!is_xen_memory() && info->flag_exclude_xen_dom) {
		MSG("'-X' option is disable,");
		MSG("because %s is not Xen's memory core image.\n", info->name_memory);
		MSG("Commandline parameter is invalid.\n");
		MSG("Try `makedumpfile --help' for more information.\n");
		return FALSE;
	}

	if (info->flag_refiltering) {
		if (info->flag_elf_dumpfile) {
			MSG("'-E' option is disable, ");
			MSG("because %s is kdump compressed format.\n",
							info->name_memory);
			return FALSE;
		}
		info->phys_base = info->kh_memory->phys_base;
		info->max_dump_level |= info->kh_memory->dump_level;

		if (!initialize_bitmap_memory())
			return FALSE;

	} else if (!get_phys_base())
		return FALSE;

	/*
	 * Get the debug information for analysis from the vmcoreinfo file
	 */
	if (info->flag_read_vmcoreinfo) {
		if (!read_vmcoreinfo())
			return FALSE;
		close_vmcoreinfo();
		debug_info = TRUE;
	/*
	 * Get the debug information for analysis from the kernel file
	 */
	} else if (info->name_vmlinux) {
		set_dwarf_debuginfo("vmlinux", NULL,
					info->name_vmlinux, info->fd_vmlinux);

		if (!get_symbol_info())
			return FALSE;

		if (!get_structure_info())
			return FALSE;

		if (!get_srcfile_info())
			return FALSE;

		debug_info = TRUE;
	} else {
		/*
		 * Check whether /proc/vmcore contains vmcoreinfo,
		 * and get both the offset and the size.
		 */
		if (!has_vmcoreinfo()) {
			if (info->max_dump_level <= DL_EXCLUDE_ZERO)
				goto out;

			MSG("%s doesn't contain vmcoreinfo.\n",
			    info->name_memory);
			MSG("Specify '-x' option or '-i' option.\n");
			MSG("Commandline parameter is invalid.\n");
			MSG("Try `makedumpfile --help' for more information.\n");
			return FALSE;
		}
	}

	/*
	 * Get the debug information from /proc/vmcore.
	 * NOTE: Don't move this code to the above, because the debugging
	 *       information token by -x/-i option is overwritten by vmcoreinfo
	 *       in /proc/vmcore. vmcoreinfo in /proc/vmcore is more reliable
	 *       than -x/-i option.
	 */
	if (has_vmcoreinfo()) {
		get_vmcoreinfo(&offset, &size);
		if (!read_vmcoreinfo_from_vmcore(offset, size, FALSE))
			return FALSE;
		debug_info = TRUE;
	}

	if (!get_value_for_old_linux())
		return FALSE;
out:
	if (!info->page_size) {
		/*
		 * If we cannot get page_size from a vmcoreinfo file,
		 * fall back to the current kernel page size.
		 */
		if (!fallback_to_current_page_size())
			return FALSE;
	}
	if (!get_max_mapnr())
		return FALSE;

	if (debug_info) {
		if (!get_machdep_info())
			return FALSE;

		if (!check_release())
			return FALSE;

		if (!get_versiondep_info())
			return FALSE;

		if (!get_numnodes())
			return FALSE;

		if (!get_mem_map())
			return FALSE;
	} else {
		if (!get_mem_map_without_mm())
			return FALSE;
	}

	return TRUE;
}

void
initialize_bitmap(struct dump_bitmap *bitmap)
{
	bitmap->fd        = info->fd_bitmap;
	bitmap->file_name = info->name_bitmap;
	bitmap->no_block  = -1;
	memset(bitmap->buf, 0, BUFSIZE_BITMAP);
}

void
initialize_1st_bitmap(struct dump_bitmap *bitmap)
{
	initialize_bitmap(bitmap);
	bitmap->offset = 0;
}

void
initialize_2nd_bitmap(struct dump_bitmap *bitmap)
{
	initialize_bitmap(bitmap);
	bitmap->offset = info->len_bitmap / 2;
}

int
set_bitmap(struct dump_bitmap *bitmap, unsigned long long pfn,
    int val)
{
	int byte, bit;
	off_t old_offset, new_offset;
	old_offset = bitmap->offset + BUFSIZE_BITMAP * bitmap->no_block;
	new_offset = bitmap->offset + BUFSIZE_BITMAP * (pfn / PFN_BUFBITMAP);

	if (0 <= bitmap->no_block && old_offset != new_offset) {
		if (lseek(bitmap->fd, old_offset, SEEK_SET) < 0 ) {
			ERRMSG("Can't seek the bitmap(%s). %s\n",
			    bitmap->file_name, strerror(errno));
			return FALSE;
		}
		if (write(bitmap->fd, bitmap->buf, BUFSIZE_BITMAP)
		    != BUFSIZE_BITMAP) {
			ERRMSG("Can't write the bitmap(%s). %s\n",
			    bitmap->file_name, strerror(errno));
			return FALSE;
		}
	}
	if (old_offset != new_offset) {
		if (lseek(bitmap->fd, new_offset, SEEK_SET) < 0 ) {
			ERRMSG("Can't seek the bitmap(%s). %s\n",
			    bitmap->file_name, strerror(errno));
			return FALSE;
		}
		if (read(bitmap->fd, bitmap->buf, BUFSIZE_BITMAP)
		    != BUFSIZE_BITMAP) {
			ERRMSG("Can't read the bitmap(%s). %s\n",
			    bitmap->file_name, strerror(errno));
			return FALSE;
		}
		bitmap->no_block = pfn / PFN_BUFBITMAP;
	}
	/*
	 * If val is 0, clear bit on the bitmap.
	 */
	byte = (pfn%PFN_BUFBITMAP)>>3;
	bit  = (pfn%PFN_BUFBITMAP) & 7;
	if (val)
		bitmap->buf[byte] |= 1<<bit;
	else
		bitmap->buf[byte] &= ~(1<<bit);

	return TRUE;
}

int
sync_bitmap(struct dump_bitmap *bitmap)
{
	off_t offset;
	offset = bitmap->offset + BUFSIZE_BITMAP * bitmap->no_block;

	/*
	 * The bitmap buffer is not dirty, and it is not necessary
	 * to write out it.
	 */
	if (bitmap->no_block < 0)
		return TRUE;

	if (lseek(bitmap->fd, offset, SEEK_SET) < 0 ) {
		ERRMSG("Can't seek the bitmap(%s). %s\n",
		    bitmap->file_name, strerror(errno));
		return FALSE;
	}
	if (write(bitmap->fd, bitmap->buf, BUFSIZE_BITMAP)
	    != BUFSIZE_BITMAP) {
		ERRMSG("Can't write the bitmap(%s). %s\n",
		    bitmap->file_name, strerror(errno));
		return FALSE;
	}
	return TRUE;
}

int
sync_1st_bitmap(void)
{
	return sync_bitmap(info->bitmap1);
}

int
sync_2nd_bitmap(void)
{
	return sync_bitmap(info->bitmap2);
}

int
set_bit_on_1st_bitmap(unsigned long long pfn)
{
	return set_bitmap(info->bitmap1, pfn, 1);
}

int
clear_bit_on_2nd_bitmap(unsigned long long pfn)
{
	return set_bitmap(info->bitmap2, pfn, 0);
}

int
clear_bit_on_2nd_bitmap_for_kernel(unsigned long long pfn)
{
	unsigned long long maddr;

	if (is_xen_memory()) {
		maddr = ptom_xen(pfn_to_paddr(pfn));
		if (maddr == NOT_PADDR) {
			ERRMSG("Can't convert a physical address(%llx) to machine address.\n",
			    pfn_to_paddr(pfn));
			return FALSE;
		}
		pfn = paddr_to_pfn(maddr);
	}
	return clear_bit_on_2nd_bitmap(pfn);
}

static inline int
is_on(char *bitmap, int i)
{
	return bitmap[i>>3] & (1 << (i & 7));
}

static inline int
is_dumpable(struct dump_bitmap *bitmap, unsigned long long pfn)
{
	off_t offset;
	if (pfn == 0 || bitmap->no_block != pfn/PFN_BUFBITMAP) {
		offset = bitmap->offset + BUFSIZE_BITMAP*(pfn/PFN_BUFBITMAP);
		lseek(bitmap->fd, offset, SEEK_SET);
		read(bitmap->fd, bitmap->buf, BUFSIZE_BITMAP);
		if (pfn == 0)
			bitmap->no_block = 0;
		else
			bitmap->no_block = pfn/PFN_BUFBITMAP;
	}
	return is_on(bitmap->buf, pfn%PFN_BUFBITMAP);
}

static inline int
is_in_segs(unsigned long long paddr)
{
	if (info->flag_refiltering) {
		static struct dump_bitmap bitmap1 = {0};

		if (bitmap1.fd == 0)
			initialize_1st_bitmap(&bitmap1);

		return is_dumpable(&bitmap1, paddr_to_pfn(paddr));
	}

	if (paddr_to_offset(paddr))
		return TRUE;
	else
		return FALSE;
}

static inline int
is_zero_page(unsigned char *buf, long page_size)
{
	size_t i;

	for (i = 0; i < page_size; i++)
		if (buf[i])
			return FALSE;
	return TRUE;
}

int
read_cache(struct cache_data *cd)
{
	const off_t failed = (off_t)-1;

	if (lseek(cd->fd, cd->offset, SEEK_SET) == failed) {
		ERRMSG("Can't seek the dump file(%s). %s\n",
		    cd->file_name, strerror(errno));
		return FALSE;
	}
	if (read(cd->fd, cd->buf, cd->cache_size) != cd->cache_size) {
		ERRMSG("Can't read the dump file(%s). %s\n",
		    cd->file_name, strerror(errno));
		return FALSE;
	}
	cd->offset += cd->cache_size;
	return TRUE;
}

int
is_bigendian(void)
{
	int i = 0x12345678;

	if (*(char *)&i == 0x12)
		return TRUE;
	else
		return FALSE;
}

int
write_and_check_space(int fd, void *buf, size_t buf_size, char *file_name)
{
	int status, written_size = 0;

	while (written_size < buf_size) {
		status = write(fd, buf + written_size,
				   buf_size - written_size);
		if (0 < status) {
			written_size += status;
			continue;
		}
		if (errno == ENOSPC)
			info->flag_nospace = TRUE;
		MSG("\nCan't write the dump file(%s). %s\n",
		    file_name, strerror(errno));
		return FALSE;
	}
	return TRUE;
}

int
write_buffer(int fd, off_t offset, void *buf, size_t buf_size, char *file_name)
{
	struct makedumpfile_data_header fdh;
	const off_t failed = (off_t)-1;

	if (fd == STDOUT_FILENO) {
		/*
		 * Output a header of flattened format instead of
		 * lseek(). For sending dump data to a different
		 * architecture, change the values to big endian.
		 */
		if (is_bigendian()){
			fdh.offset   = offset;
			fdh.buf_size = buf_size;
		} else {
			fdh.offset   = bswap_64(offset);
			fdh.buf_size = bswap_64(buf_size);
		}
		if (!write_and_check_space(fd, &fdh, sizeof(fdh), file_name))
			return FALSE;
	} else {
		if (lseek(fd, offset, SEEK_SET) == failed) {
			ERRMSG("Can't seek the dump file(%s). %s\n",
			    file_name, strerror(errno));
			return FALSE;
		}
	}
	if (!write_and_check_space(fd, buf, buf_size, file_name))
		return FALSE;

	return TRUE;
}

int
write_cache(struct cache_data *cd, void *buf, size_t size)
{
	memcpy(cd->buf + cd->buf_size, buf, size);
	cd->buf_size += size;

	if (cd->buf_size < cd->cache_size)
		return TRUE;

	if (!write_buffer(cd->fd, cd->offset, cd->buf, cd->cache_size,
	    cd->file_name))
		return FALSE;

	cd->buf_size -= cd->cache_size;
	memcpy(cd->buf, cd->buf + cd->cache_size, cd->buf_size);
	cd->offset += cd->cache_size;
	return TRUE;
}

int
write_cache_bufsz(struct cache_data *cd)
{
	if (!cd->buf_size)
		return TRUE;

	if (!write_buffer(cd->fd, cd->offset, cd->buf, cd->buf_size,
	    cd->file_name))
		return FALSE;

	cd->offset  += cd->buf_size;
	cd->buf_size = 0;
	return TRUE;
}

int
write_cache_zero(struct cache_data *cd, size_t size)
{
	if (!write_cache_bufsz(cd))
		return FALSE;

	memset(cd->buf + cd->buf_size, 0, size);
	cd->buf_size += size;

	return write_cache_bufsz(cd);
}

int
read_buf_from_stdin(void *buf, int buf_size)
{
	int read_size = 0, tmp_read_size = 0;
	time_t last_time, tm;

	last_time = time(NULL);

	while (read_size != buf_size) {

		tmp_read_size = read(STDIN_FILENO, buf + read_size,
		    buf_size - read_size);

		if (tmp_read_size < 0) {
			ERRMSG("Can't read STDIN. %s\n", strerror(errno));
			return FALSE;

		} else if (0 == tmp_read_size) {
			/*
			 * If it cannot get any data from a standard input
			 * for a long time, break this loop.
			 */
			tm = time(NULL);
			if (TIMEOUT_STDIN < (tm - last_time)) {
				ERRMSG("Can't get any data from STDIN.\n");
				return FALSE;
			}
		} else {
			read_size += tmp_read_size;
			last_time = time(NULL);
		}
	}
	return TRUE;
}

int
read_start_flat_header(void)
{
	char buf[MAX_SIZE_MDF_HEADER];
	struct makedumpfile_header fh;

	/*
	 * Get flat header.
	 */
	if (!read_buf_from_stdin(buf, MAX_SIZE_MDF_HEADER)) {
		ERRMSG("Can't get header of flattened format.\n");
		return FALSE;
	}
	memcpy(&fh, buf, sizeof(fh));

	if (!is_bigendian()){
		fh.type    = bswap_64(fh.type);
		fh.version = bswap_64(fh.version);
	}

	/*
	 * Check flat header.
	 */
	if (strcmp(fh.signature, MAKEDUMPFILE_SIGNATURE)) {
		ERRMSG("Can't get signature of flattened format.\n");
		return FALSE;
	}
	if (fh.type != TYPE_FLAT_HEADER) {
		ERRMSG("Can't get type of flattened format.\n");
		return FALSE;
	}

	return TRUE;
}

int
read_flat_data_header(struct makedumpfile_data_header *fdh)
{
	if (!read_buf_from_stdin(fdh,
	    sizeof(struct makedumpfile_data_header))) {
		ERRMSG("Can't get header of flattened format.\n");
		return FALSE;
	}
	if (!is_bigendian()){
		fdh->offset   = bswap_64(fdh->offset);
		fdh->buf_size = bswap_64(fdh->buf_size);
	}
	return TRUE;
}

int
rearrange_dumpdata(void)
{
	int read_size, tmp_read_size;
	char buf[SIZE_BUF_STDIN];
	struct makedumpfile_data_header fdh;

	/*
	 * Get flat header.
	 */
	if (!read_start_flat_header()) {
		ERRMSG("Can't get header of flattened format.\n");
		return FALSE;
	}

	/*
	 * Read the first data header.
	 */
	if (!read_flat_data_header(&fdh)) {
		ERRMSG("Can't get header of flattened format.\n");
		return FALSE;
	}

	do {
		read_size = 0;
		while (read_size < fdh.buf_size) {
			if (sizeof(buf) < (fdh.buf_size - read_size))
				tmp_read_size = sizeof(buf);
			else
				tmp_read_size = fdh.buf_size - read_size;

			if (!read_buf_from_stdin(buf, tmp_read_size)) {
				ERRMSG("Can't get data of flattened format.\n");
				return FALSE;
			}
			if (!write_buffer(info->fd_dumpfile,
			    fdh.offset + read_size, buf, tmp_read_size,
			    info->name_dumpfile))
				return FALSE;

			read_size += tmp_read_size;
		}
		/*
		 * Read the next header.
		 */
		if (!read_flat_data_header(&fdh)) {
			ERRMSG("Can't get data header of flattened format.\n");
			return FALSE;
		}

	} while ((0 <= fdh.offset) && (0 < fdh.buf_size));

	if ((fdh.offset != END_FLAG_FLAT_HEADER)
	    || (fdh.buf_size != END_FLAG_FLAT_HEADER)) {
		ERRMSG("Can't get valid end header of flattened format.\n");
		return FALSE;
	}

	return TRUE;
}

unsigned long long
page_to_pfn(unsigned long page)
{
	unsigned int num;
	unsigned long long pfn = ULONGLONG_MAX;
	unsigned long long index = 0;
	struct mem_map_data *mmd;

	mmd = info->mem_map_data;
	for (num = 0; num < info->num_mem_map; num++, mmd++) {
		if (mmd->mem_map == NOT_MEMMAP_ADDR)
			continue;
		if (page < mmd->mem_map)
			continue;
		index = (page - mmd->mem_map) / SIZE(page);
		if (index > mmd->pfn_end - mmd->pfn_start)
			continue;
		pfn = mmd->pfn_start + index;
		break;
	}
	if (pfn == ULONGLONG_MAX) {
		ERRMSG("Can't convert the address of page descriptor (%lx) to pfn.\n", page);
		return ULONGLONG_MAX;
	}
	return pfn;
}

int
reset_bitmap_of_free_pages(unsigned long node_zones)
{

	int order, i, migrate_type, migrate_types;
	unsigned long curr, previous, head, curr_page, curr_prev;
	unsigned long addr_free_pages, free_pages = 0, found_free_pages = 0;
	unsigned long long pfn, start_pfn;

	/*
	 * On linux-2.6.24 or later, free_list is divided into the array.
	 */
	migrate_types = ARRAY_LENGTH(free_area.free_list);
	if (migrate_types == NOT_FOUND_STRUCTURE)
		migrate_types = 1;

	for (order = (ARRAY_LENGTH(zone.free_area) - 1); order >= 0; --order) {
		for (migrate_type = 0; migrate_type < migrate_types;
		     migrate_type++) {
			head = node_zones + OFFSET(zone.free_area)
				+ SIZE(free_area) * order
				+ OFFSET(free_area.free_list)
				+ SIZE(list_head) * migrate_type;
			previous = head;
			if (!readmem(VADDR, head + OFFSET(list_head.next),
				     &curr, sizeof curr)) {
				ERRMSG("Can't get next list_head.\n");
				return FALSE;
			}
			for (;curr != head;) {
				curr_page = curr - OFFSET(page.lru);
				start_pfn = page_to_pfn(curr_page);
				if (start_pfn == ULONGLONG_MAX)
					return FALSE;

				if (!readmem(VADDR, curr+OFFSET(list_head.prev),
					     &curr_prev, sizeof curr_prev)) {
					ERRMSG("Can't get prev list_head.\n");
					return FALSE;
				}
				if (previous != curr_prev) {
					ERRMSG("The free list is broken.\n");
					retcd = ANALYSIS_FAILED;
					return FALSE;
				}
				for (i = 0; i < (1<<order); i++) {
					pfn = start_pfn + i;
					clear_bit_on_2nd_bitmap_for_kernel(pfn);
				}
				found_free_pages += i;

				previous = curr;
				if (!readmem(VADDR, curr+OFFSET(list_head.next),
					     &curr, sizeof curr)) {
					ERRMSG("Can't get next list_head.\n");
					return FALSE;
				}
			}
		}
	}

	/*
	 * Check the number of free pages.
	 */
	if (OFFSET(zone.free_pages) != NOT_FOUND_STRUCTURE) {
		addr_free_pages = node_zones + OFFSET(zone.free_pages);

	} else if (OFFSET(zone.vm_stat) != NOT_FOUND_STRUCTURE) {
		/*
		 * On linux-2.6.21 or later, the number of free_pages is
		 * in vm_stat[NR_FREE_PAGES].
		 */
		addr_free_pages = node_zones + OFFSET(zone.vm_stat)
		    + sizeof(long) * NUMBER(NR_FREE_PAGES);

	} else {
		ERRMSG("Can't get addr_free_pages.\n");
		return FALSE;
	}
	if (!readmem(VADDR, addr_free_pages, &free_pages, sizeof free_pages)) {
		ERRMSG("Can't get free_pages.\n");
		return FALSE;
	}
	if (free_pages != found_free_pages) {
		/*
		 * On linux-2.6.21 or later, the number of free_pages is
		 * sometimes different from the one of the list "free_area",
		 * because the former is flushed asynchronously.
		 */
		DEBUG_MSG("The number of free_pages is invalid.\n");
		DEBUG_MSG("  free_pages       = %ld\n", free_pages);
		DEBUG_MSG("  found_free_pages = %ld\n", found_free_pages);
	}
	pfn_free += found_free_pages;

	return TRUE;
}

int
dump_dmesg()
{
	int log_buf_len, length_log, length_oldlog, ret = FALSE;
	unsigned long log_buf, log_end, index;
	unsigned long log_end_2_6_24;
	unsigned      log_end_2_6_25;
	char *log_buffer = NULL;

	/*
	 * log_end has been changed to "unsigned" since linux-2.6.25.
	 *   2.6.24 or former: static unsigned long log_end;
	 *   2.6.25 or later : static unsigned log_end;
	 */
	if (!open_files_for_creating_dumpfile())
		return FALSE;

	if (!info->flag_refiltering) {
		if (!get_elf_info(info->fd_memory, info->name_memory))
			return FALSE;
	}
	if (!initial())
		return FALSE;

	if ((SYMBOL(log_buf) == NOT_FOUND_SYMBOL)
	    || (SYMBOL(log_buf_len) == NOT_FOUND_SYMBOL)
	    || (SYMBOL(log_end) == NOT_FOUND_SYMBOL)) {
		ERRMSG("Can't find some symbols for log_buf.\n");
		return FALSE;
	}
	if (!readmem(VADDR, SYMBOL(log_buf), &log_buf, sizeof(log_buf))) {
		ERRMSG("Can't get log_buf.\n");
		return FALSE;
	}
	if (info->kernel_version >= KERNEL_VERSION(2, 6, 25)) {
		if (!readmem(VADDR, SYMBOL(log_end), &log_end_2_6_25,
		    sizeof(log_end_2_6_25))) {
			ERRMSG("Can't to get log_end.\n");
			return FALSE;
		}
		log_end = log_end_2_6_25;
	} else {
		if (!readmem(VADDR, SYMBOL(log_end), &log_end_2_6_24,
		    sizeof(log_end_2_6_24))) {
			ERRMSG("Can't to get log_end.\n");
			return FALSE;
		}
		log_end = log_end_2_6_24;
	}
	if (!readmem(VADDR, SYMBOL(log_buf_len), &log_buf_len,
	    sizeof(log_buf_len))) {
		ERRMSG("Can't get log_buf_len.\n");
		return FALSE;
	}
	DEBUG_MSG("\n");
	DEBUG_MSG("log_buf      : %lx\n", log_buf);
	DEBUG_MSG("log_end      : %lx\n", log_end);
	DEBUG_MSG("log_buf_len  : %d\n", log_buf_len);

	if ((log_buffer = malloc(log_buf_len)) == NULL) {
		ERRMSG("Can't allocate memory for log_buf. %s\n",
		    strerror(errno));
		return FALSE;
	}

	if (log_end < log_buf_len) {
		length_log = log_end;
		if(!readmem(VADDR, log_buf, log_buffer, length_log)) {
			ERRMSG("Can't read dmesg log.\n");
			goto out;
		}
	} else {
		index = log_end & (log_buf_len - 1);
		DEBUG_MSG("index        : %lx\n", index);
		length_log = log_buf_len;
		length_oldlog = log_buf_len - index;
		if(!readmem(VADDR, log_buf + index, log_buffer, length_oldlog)) {
			ERRMSG("Can't read old dmesg log.\n");
			goto out;
		}
		if(!readmem(VADDR, log_buf, log_buffer + length_oldlog, index)) {
			ERRMSG("Can't read new dmesg log.\n");
			goto out;
		}
	}
	DEBUG_MSG("length_log   : %d\n", length_log);

	if (!open_dump_file()) {
		ERRMSG("Can't open output file.\n");
		goto out;
	}
	if (write(info->fd_dumpfile, log_buffer, length_log) < 0)
		goto out;

	if (!close_files_for_creating_dumpfile())
		goto out;

	ret = TRUE;
out:
	if (log_buffer)
		free(log_buffer);

	return ret;
}


int
_exclude_free_page(void)
{
	int i, nr_zones, num_nodes, node;
	unsigned long node_zones, zone, spanned_pages, pgdat;
	struct timeval tv_start;

	if ((node = next_online_node(0)) < 0) {
		ERRMSG("Can't get next online node.\n");
		return FALSE;
	}
	if (!(pgdat = next_online_pgdat(node))) {
		ERRMSG("Can't get pgdat list.\n");
		return FALSE;
	}
	gettimeofday(&tv_start, NULL);

	for (num_nodes = 1; num_nodes <= vt.numnodes; num_nodes++) {

		print_progress(PROGRESS_FREE_PAGES, num_nodes - 1, vt.numnodes);

		node_zones = pgdat + OFFSET(pglist_data.node_zones);

		if (!readmem(VADDR, pgdat + OFFSET(pglist_data.nr_zones),
		    &nr_zones, sizeof(nr_zones))) {
			ERRMSG("Can't get nr_zones.\n");
			return FALSE;
		}

		for (i = 0; i < nr_zones; i++) {

			print_progress(PROGRESS_FREE_PAGES, i + nr_zones * (num_nodes - 1),
					nr_zones * vt.numnodes);

			zone = node_zones + (i * SIZE(zone));
			if (!readmem(VADDR, zone + OFFSET(zone.spanned_pages),
			    &spanned_pages, sizeof spanned_pages)) {
				ERRMSG("Can't get spanned_pages.\n");
				return FALSE;
			}
			if (!spanned_pages)
				continue;
			if (!reset_bitmap_of_free_pages(zone))
				return FALSE;
		}
		if (num_nodes < vt.numnodes) {
			if ((node = next_online_node(node + 1)) < 0) {
				ERRMSG("Can't get next online node.\n");
				return FALSE;
			} else if (!(pgdat = next_online_pgdat(node))) {
				ERRMSG("Can't determine pgdat list (node %d).\n",
				    node);
				return FALSE;
			}
		}
	}

	/*
	 * print [100 %]
	 */
	print_progress(PROGRESS_FREE_PAGES, vt.numnodes, vt.numnodes);
	print_execution_time(PROGRESS_FREE_PAGES, &tv_start);

	return TRUE;
}

int
exclude_free_page(void)
{
	/*
	 * Check having necessary information.
	 */
	if ((SYMBOL(node_data) == NOT_FOUND_SYMBOL)
	    && (SYMBOL(pgdat_list) == NOT_FOUND_SYMBOL)
	    && (SYMBOL(contig_page_data) == NOT_FOUND_SYMBOL)) {
		ERRMSG("Can't get necessary symbols for excluding free pages.\n");
		return FALSE;
	}
	if ((SIZE(zone) == NOT_FOUND_STRUCTURE)
	    || ((OFFSET(zone.free_pages) == NOT_FOUND_STRUCTURE)
	        && (OFFSET(zone.vm_stat) == NOT_FOUND_STRUCTURE))
	    || (OFFSET(zone.free_area) == NOT_FOUND_STRUCTURE)
	    || (OFFSET(zone.spanned_pages) == NOT_FOUND_STRUCTURE)
	    || (OFFSET(pglist_data.node_zones) == NOT_FOUND_STRUCTURE)
	    || (OFFSET(pglist_data.nr_zones) == NOT_FOUND_STRUCTURE)
	    || (SIZE(free_area) == NOT_FOUND_STRUCTURE)
	    || (OFFSET(free_area.free_list) == NOT_FOUND_STRUCTURE)
	    || (OFFSET(list_head.next) == NOT_FOUND_STRUCTURE)
	    || (OFFSET(list_head.prev) == NOT_FOUND_STRUCTURE)
	    || (OFFSET(page.lru) == NOT_FOUND_STRUCTURE)
	    || (ARRAY_LENGTH(zone.free_area) == NOT_FOUND_STRUCTURE)) {
		ERRMSG("Can't get necessary structures for excluding free pages.\n");
		return FALSE;
	}

	/*
	 * Detect free pages and update 2nd-bitmap.
	 */
	if (!_exclude_free_page())
		return FALSE;

	return TRUE;
}

/*
 * If using a dumpfile in kdump-compressed format as a source file
 * instead of /proc/vmcore, 1st-bitmap of a new dumpfile must be
 * the same as the one of a source file.
 */
int
copy_1st_bitmap_from_memory(void)
{
	char buf[info->dh_memory->block_size];
	off_t offset_page;
	off_t bitmap_offset;
	struct disk_dump_header *dh = info->dh_memory;

	bitmap_offset = (DISKDUMP_HEADER_BLOCKS + dh->sub_hdr_size)
			 * dh->block_size;

	if (lseek(info->fd_memory, bitmap_offset, SEEK_SET) < 0) {
		ERRMSG("Can't seek %s. %s\n",
				info->name_memory, strerror(errno));
		return FALSE;
	}
	if (lseek(info->bitmap1->fd, info->bitmap1->offset, SEEK_SET) < 0) {
		ERRMSG("Can't seek the bitmap(%s). %s\n",
		    info->bitmap1->file_name, strerror(errno));
		return FALSE;
	}
	offset_page = 0;
	while (offset_page < (info->len_bitmap / 2)) {
		if (read(info->fd_memory, buf, sizeof(buf)) != sizeof(buf)) {
			ERRMSG("Can't read %s. %s\n",
					info->name_memory, strerror(errno));
			return FALSE;
		}
		if (write(info->bitmap1->fd, buf, sizeof(buf)) != sizeof(buf)) {
			ERRMSG("Can't write the bitmap(%s). %s\n",
			    info->bitmap1->file_name, strerror(errno));
			return FALSE;
		}
		offset_page += sizeof(buf);
	}
	return TRUE;
}

int
create_1st_bitmap(void)
{
	int i;
	unsigned int num_pt_loads = get_num_pt_loads();
 	char buf[info->page_size];
	unsigned long long pfn, pfn_start, pfn_end, pfn_bitmap1;
	unsigned long long phys_start, phys_end;
	struct timeval tv_start;
	off_t offset_page;

	if (info->flag_refiltering)
		return copy_1st_bitmap_from_memory();

	/*
	 * At first, clear all the bits on the 1st-bitmap.
	 */
	memset(buf, 0, sizeof(buf));

	if (lseek(info->bitmap1->fd, info->bitmap1->offset, SEEK_SET) < 0) {
		ERRMSG("Can't seek the bitmap(%s). %s\n",
		    info->bitmap1->file_name, strerror(errno));
		return FALSE;
	}
	offset_page = 0;
	while (offset_page < (info->len_bitmap / 2)) {
		if (write(info->bitmap1->fd, buf, info->page_size)
		    != info->page_size) {
			ERRMSG("Can't write the bitmap(%s). %s\n",
			    info->bitmap1->file_name, strerror(errno));
			return FALSE;
		}
		offset_page += info->page_size;
	}

	gettimeofday(&tv_start, NULL);

	/*
	 * If page is on memory hole, set bit on the 1st-bitmap.
	 */
	pfn_bitmap1 = 0;
	for (i = 0; get_pt_load(i, &phys_start, &phys_end, NULL, NULL); i++) {

		print_progress(PROGRESS_HOLES, i, num_pt_loads);

		pfn_start = paddr_to_pfn(phys_start);
		pfn_end   = paddr_to_pfn(phys_end);

		if (!is_in_segs(pfn_to_paddr(pfn_start)))
			pfn_start++;
		for (pfn = pfn_start; pfn < pfn_end; pfn++) {
			set_bit_on_1st_bitmap(pfn);
			pfn_bitmap1++;
		}
	}
	pfn_memhole = info->max_mapnr - pfn_bitmap1;

	/*
	 * print 100 %
	 */
	print_progress(PROGRESS_HOLES, info->max_mapnr, info->max_mapnr);
	print_execution_time(PROGRESS_HOLES, &tv_start);

	if (!sync_1st_bitmap())
		return FALSE;

	return TRUE;
}

/*
 * Exclude the page filled with zero in case of creating an elf dumpfile.
 */
int
exclude_zero_pages(void)
{
	unsigned long long pfn, paddr;
	struct dump_bitmap bitmap2;
	struct timeval tv_start;
	unsigned char buf[info->page_size];

	initialize_2nd_bitmap(&bitmap2);

	gettimeofday(&tv_start, NULL);

	for (pfn = 0, paddr = pfn_to_paddr(pfn); pfn < info->max_mapnr;
	    pfn++, paddr += info->page_size) {

		print_progress(PROGRESS_ZERO_PAGES, pfn, info->max_mapnr);

		if (!is_in_segs(paddr))
			continue;

		if (!is_dumpable(&bitmap2, pfn))
			continue;

		if (is_xen_memory()) {
			if (!readmem(MADDR_XEN, paddr, buf, info->page_size)) {
				ERRMSG("Can't get the page data(pfn:%llx, max_mapnr:%llx).\n",
				    pfn, info->max_mapnr);
				return FALSE;
			}
		} else {
			if (!readmem(PADDR, paddr, buf, info->page_size)) {
				ERRMSG("Can't get the page data(pfn:%llx, max_mapnr:%llx).\n",
				    pfn, info->max_mapnr);
				return FALSE;
			}
		}
		if (is_zero_page(buf, info->page_size)) {
			clear_bit_on_2nd_bitmap(pfn);
			pfn_zero++;
		}
	}

	/*
	 * print [100 %]
	 */
	print_progress(PROGRESS_ZERO_PAGES, info->max_mapnr, info->max_mapnr);
	print_execution_time(PROGRESS_ZERO_PAGES, &tv_start);

	return TRUE;
}

int
__exclude_unnecessary_pages(unsigned long mem_map,
    unsigned long long pfn_start, unsigned long long pfn_end)
{
	unsigned long long pfn, pfn_mm, maddr;
	unsigned long long pfn_read_start, pfn_read_end, index_pg;
	unsigned char page_cache[SIZE(page) * PGMM_CACHED];
	unsigned char *pcache;
	unsigned int _count;
	unsigned long flags, mapping;

	/*
	 * Refresh the buffer of struct page, when changing mem_map.
	 */
	pfn_read_start = ULONGLONG_MAX;
	pfn_read_end   = 0;

	for (pfn = pfn_start; pfn < pfn_end; pfn++, mem_map += SIZE(page)) {

		/*
		 * Exclude the memory hole.
		 */
		if (is_xen_memory()) {
			maddr = ptom_xen(pfn_to_paddr(pfn));
			if (maddr == NOT_PADDR) {
				ERRMSG("Can't convert a physical address(%llx) to machine address.\n",
				    pfn_to_paddr(pfn));
				return FALSE;
			}
			if (!is_in_segs(maddr))
				continue;
		} else {
			if (!is_in_segs(pfn_to_paddr(pfn)))
				continue;
		}

		index_pg = pfn % PGMM_CACHED;
		if (pfn < pfn_read_start || pfn_read_end < pfn) {
			if (roundup(pfn + 1, PGMM_CACHED) < pfn_end)
				pfn_mm = PGMM_CACHED - index_pg;
			else
				pfn_mm = pfn_end - pfn;

			if (!readmem(VADDR, mem_map,
			    page_cache + (index_pg * SIZE(page)),
			    SIZE(page) * pfn_mm)) {
				ERRMSG("Can't read the buffer of struct page.\n");
				return FALSE;
			}
			pfn_read_start = pfn;
			pfn_read_end   = pfn + pfn_mm - 1;
		}
		pcache  = page_cache + (index_pg * SIZE(page));

		flags   = ULONG(pcache + OFFSET(page.flags));
		_count  = UINT(pcache + OFFSET(page._count));
		mapping = ULONG(pcache + OFFSET(page.mapping));

		/*
		 * Exclude the cache page without the private page.
		 */
		if ((info->dump_level & DL_EXCLUDE_CACHE)
		    && (isLRU(flags) || isSwapCache(flags))
		    && !isPrivate(flags) && !isAnon(mapping)) {
			clear_bit_on_2nd_bitmap_for_kernel(pfn);
			pfn_cache++;
		}
		/*
		 * Exclude the cache page with the private page.
		 */
		else if ((info->dump_level & DL_EXCLUDE_CACHE_PRI)
		    && (isLRU(flags) || isSwapCache(flags))
		    && !isAnon(mapping)) {
			clear_bit_on_2nd_bitmap_for_kernel(pfn);
			pfn_cache_private++;
		}
		/*
		 * Exclude the data page of the user process.
		 */
		else if ((info->dump_level & DL_EXCLUDE_USER_DATA)
		    && isAnon(mapping)) {
			clear_bit_on_2nd_bitmap_for_kernel(pfn);
			pfn_user++;
		}
	}
	return TRUE;
}

int
exclude_unnecessary_pages(void)
{
	unsigned int mm;
	struct mem_map_data *mmd;
	struct timeval tv_start;

	gettimeofday(&tv_start, NULL);

	for (mm = 0; mm < info->num_mem_map; mm++) {
		print_progress(PROGRESS_UNN_PAGES, mm, info->num_mem_map);

		mmd = &info->mem_map_data[mm];

		if (mmd->mem_map == NOT_MEMMAP_ADDR)
			continue;

		if (!__exclude_unnecessary_pages(mmd->mem_map,
						 mmd->pfn_start, mmd->pfn_end))
			return FALSE;
	}

	/*
	 * print [100 %]
	 */
	print_progress(PROGRESS_UNN_PAGES, info->num_mem_map, info->num_mem_map);
	print_execution_time(PROGRESS_UNN_PAGES, &tv_start);

	return TRUE;
}

int
copy_bitmap(void)
{
	off_t offset;
	unsigned char buf[info->page_size];
 	const off_t failed = (off_t)-1;

	offset = 0;
	while (offset < (info->len_bitmap / 2)) {
		if (lseek(info->bitmap1->fd, info->bitmap1->offset + offset,
		    SEEK_SET) == failed) {
			ERRMSG("Can't seek the bitmap(%s). %s\n",
			    info->name_bitmap, strerror(errno));
			return FALSE;
		}
		if (read(info->bitmap1->fd, buf, sizeof(buf)) != sizeof(buf)) {
			ERRMSG("Can't read the dump memory(%s). %s\n",
			    info->name_memory, strerror(errno));
			return FALSE;
		}
		if (lseek(info->bitmap2->fd, info->bitmap2->offset + offset,
		    SEEK_SET) == failed) {
			ERRMSG("Can't seek the bitmap(%s). %s\n",
			    info->name_bitmap, strerror(errno));
			return FALSE;
		}
		if (write(info->bitmap2->fd, buf, sizeof(buf)) != sizeof(buf)) {
			ERRMSG("Can't write the bitmap(%s). %s\n",
		    	info->name_bitmap, strerror(errno));
			return FALSE;
		}
		offset += sizeof(buf);
	}

	return TRUE;
}

int
create_2nd_bitmap(void)
{
	/*
	 * Copy 1st-bitmap to 2nd-bitmap.
	 */
	if (!copy_bitmap()) {
		ERRMSG("Can't copy 1st-bitmap to 2nd-bitmap.\n");
		return FALSE;
	}

	/*
	 * Exclude cache pages, cache private pages, user data pages.
	 */
	if (info->dump_level & DL_EXCLUDE_CACHE ||
	    info->dump_level & DL_EXCLUDE_CACHE_PRI ||
	    info->dump_level & DL_EXCLUDE_USER_DATA) {
		if (!exclude_unnecessary_pages()) {
			ERRMSG("Can't exclude unnecessary pages.\n");
			return FALSE;
		}
	}

	/*
	 * Exclude free pages.
	 */
	if (info->dump_level & DL_EXCLUDE_FREE)
		if (!exclude_free_page())
			return FALSE;

	/*
	 * Exclude Xen user domain.
	 */
	if (info->flag_exclude_xen_dom) {
		if (!exclude_xen_user_domain()) {
			ERRMSG("Can't exclude xen user domain.\n");
			return FALSE;
		}
	}

	/*
	 * Exclude pages filled with zero for creating an ELF dumpfile.
	 *
	 * Note: If creating a kdump-compressed dumpfile, makedumpfile
	 *	 checks zero-pages while copying dumpable pages to a
	 *	 dumpfile from /proc/vmcore. That is valuable for the
	 *	 speed, because each page is read one time only.
	 *	 Otherwise (if creating an ELF dumpfile), makedumpfile
	 *	 should check zero-pages at this time because 2nd-bitmap
	 *	 should be fixed for creating an ELF header. That is slow
	 *	 due to reading each page two times, but it is necessary.
	 */
	if ((info->dump_level & DL_EXCLUDE_ZERO) && info->flag_elf_dumpfile) {
		/*
		 * 2nd-bitmap should be flushed at this time, because
		 * exclude_zero_pages() checks 2nd-bitmap.
		 */
		if (!sync_2nd_bitmap())
			return FALSE;

		if (!exclude_zero_pages()) {
			ERRMSG("Can't exclude pages filled with zero for creating an ELF dumpfile.\n");
			return FALSE;
		}
	}

	if (!sync_2nd_bitmap())
		return FALSE;

	return TRUE;
}

int
prepare_bitmap_buffer(void)
{
	unsigned long tmp;

	/*
	 * Create 2 bitmaps (1st-bitmap & 2nd-bitmap) on block_size boundary.
	 * The crash utility requires both of them to be aligned to block_size
	 * boundary.
	 */
	tmp = divideup(divideup(info->max_mapnr, BITPERBYTE), info->page_size);
	info->len_bitmap = tmp*info->page_size*2;

	/*
	 * Prepare bitmap buffers for creating dump bitmap.
	 */
	if ((info->bitmap1 = malloc(sizeof(struct dump_bitmap))) == NULL) {
		ERRMSG("Can't allocate memory for the 1st-bitmap. %s\n",
		    strerror(errno));
		return FALSE;
	}
	if ((info->bitmap2 = malloc(sizeof(struct dump_bitmap))) == NULL) {
		ERRMSG("Can't allocate memory for the 2nd-bitmap. %s\n",
		    strerror(errno));
		return FALSE;
	}
	initialize_1st_bitmap(info->bitmap1);
	initialize_2nd_bitmap(info->bitmap2);

	return TRUE;
}

void
free_bitmap_buffer(void)
{
	if (info->bitmap1) {
		free(info->bitmap1);
		info->bitmap1 = NULL;
	}
	if (info->bitmap2) {
		free(info->bitmap2);
		info->bitmap2 = NULL;
	}

	return;
}

int
create_dump_bitmap(void)
{
	int ret = FALSE;

	if (!prepare_bitmap_buffer())
		goto out;

	if (!create_1st_bitmap())
		goto out;

	if (!create_2nd_bitmap())
		goto out;

	ret = TRUE;
out:
	free_bitmap_buffer();

	return ret;
}

int
get_loads_dumpfile(void)
{
	int i, phnum, num_new_load = 0;
	long page_size = info->page_size;
	unsigned long long pfn, pfn_start, pfn_end, num_excluded;
	unsigned long frac_head, frac_tail;
	Elf64_Phdr load;
	struct dump_bitmap bitmap2;

	initialize_2nd_bitmap(&bitmap2);

	if (!(phnum = get_phnum_memory()))
		return FALSE;

	for (i = 0; i < phnum; i++) {
		if (!get_phdr_memory(i, &load))
			return FALSE;
		if (load.p_type != PT_LOAD)
			continue;

		pfn_start = paddr_to_pfn(load.p_paddr);
		pfn_end   = paddr_to_pfn(load.p_paddr + load.p_memsz);
		frac_head = page_size - (load.p_paddr % page_size);
		frac_tail = (load.p_paddr + load.p_memsz) % page_size;

		num_new_load++;
		num_excluded = 0;

		if (frac_head && (frac_head != page_size))
			pfn_start++;
		if (frac_tail)
			pfn_end++;

		for (pfn = pfn_start; pfn < pfn_end; pfn++) {
			if (!is_dumpable(&bitmap2, pfn)) {
				num_excluded++;
				continue;
			}

			/*
			 * If the number of the contiguous pages to be excluded
			 * is 256 or more, those pages are excluded really.
			 * And a new PT_LOAD segment is created.
			 */
			if (num_excluded >= PFN_EXCLUDED) {
				num_new_load++;
			}
			num_excluded = 0;
		}
	}
	return num_new_load;
}

int
prepare_cache_data(struct cache_data *cd)
{
	cd->fd         = info->fd_dumpfile;
	cd->file_name  = info->name_dumpfile;
	cd->cache_size = info->page_size << info->block_order;
	cd->buf_size   = 0;
	cd->buf        = NULL;

	if ((cd->buf = malloc(cd->cache_size + info->page_size)) == NULL) {
		ERRMSG("Can't allocate memory for the data buffer. %s\n",
		    strerror(errno));
		return FALSE;
	}
	return TRUE;
}

void
free_cache_data(struct cache_data *cd)
{
	free(cd->buf);
	cd->buf = NULL;
}

int
write_start_flat_header()
{
	char buf[MAX_SIZE_MDF_HEADER];
	struct makedumpfile_header fh;

	if (!info->flag_flatten)
		return FALSE;

	strcpy(fh.signature, MAKEDUMPFILE_SIGNATURE);

	/*
	 * For sending dump data to a different architecture, change the values
	 * to big endian.
	 */
	if (is_bigendian()){
		fh.type    = TYPE_FLAT_HEADER;
		fh.version = VERSION_FLAT_HEADER;
	} else {
		fh.type    = bswap_64(TYPE_FLAT_HEADER);
		fh.version = bswap_64(VERSION_FLAT_HEADER);
	}

	memset(buf, 0, sizeof(buf));
	memcpy(buf, &fh, sizeof(fh));

	if (!write_and_check_space(info->fd_dumpfile, buf, MAX_SIZE_MDF_HEADER,
	    info->name_dumpfile))
		return FALSE;

	return TRUE;
}

int
write_end_flat_header(void)
{
	struct makedumpfile_data_header fdh;

	if (!info->flag_flatten)
		return FALSE;

	fdh.offset   = END_FLAG_FLAT_HEADER;
	fdh.buf_size = END_FLAG_FLAT_HEADER;

	if (!write_and_check_space(info->fd_dumpfile, &fdh, sizeof(fdh),
	    info->name_dumpfile))
		return FALSE;

	return TRUE;
}

int
write_elf_phdr(struct cache_data *cd_hdr, Elf64_Phdr *load)
{
	Elf32_Phdr load32;

	if (is_elf64_memory()) { /* ELF64 */
		if (!write_cache(cd_hdr, load, sizeof(Elf64_Phdr)))
			return FALSE;

	} else {
		memset(&load32, 0, sizeof(Elf32_Phdr));
		load32.p_type   = load->p_type;
		load32.p_flags  = load->p_flags;
		load32.p_offset = load->p_offset;
		load32.p_vaddr  = load->p_vaddr;
		load32.p_paddr  = load->p_paddr;
		load32.p_filesz = load->p_filesz;
		load32.p_memsz  = load->p_memsz;
		load32.p_align  = load->p_align;

		if (!write_cache(cd_hdr, &load32, sizeof(Elf32_Phdr)))
			return FALSE;
	}
	return TRUE;
}

int
write_elf_header(struct cache_data *cd_header)
{
	int i, num_loads_dumpfile, phnum;
	off_t offset_note_memory, offset_note_dumpfile;
	size_t size_note, size_eraseinfo = 0;
	Elf64_Ehdr ehdr64;
	Elf32_Ehdr ehdr32;
	Elf64_Phdr note;
	char size_str[MAX_SIZE_STR_LEN];
	struct filter_info *fl_info = filter_info;

	char *buf = NULL;
	const off_t failed = (off_t)-1;

	int ret = FALSE;

	if (!info->flag_elf_dumpfile)
		return FALSE;

	/*
	 * Get the PT_LOAD number of the dumpfile.
	 */
	if (!(num_loads_dumpfile = get_loads_dumpfile())) {
		ERRMSG("Can't get a number of PT_LOAD.\n");
		goto out;
	}

	if (is_elf64_memory()) { /* ELF64 */
		if (!get_elf64_ehdr(info->fd_memory,
				    info->name_memory, &ehdr64)) {
			ERRMSG("Can't get ehdr64.\n");
			goto out;
		}
		/*
		 * PT_NOTE(1) + PT_LOAD(1+)
		 */
		ehdr64.e_phnum = 1 + num_loads_dumpfile;
	} else {                /* ELF32 */
		if (!get_elf32_ehdr(info->fd_memory,
				    info->name_memory, &ehdr32)) {
			ERRMSG("Can't get ehdr32.\n");
			goto out;
		}
		/*
		 * PT_NOTE(1) + PT_LOAD(1+)
		 */
		ehdr32.e_phnum = 1 + num_loads_dumpfile;
	}

	/*
	 * Write an ELF header.
	 */
	if (is_elf64_memory()) { /* ELF64 */
		if (!write_buffer(info->fd_dumpfile, 0, &ehdr64, sizeof(ehdr64),
		    info->name_dumpfile))
			goto out;

	} else {                /* ELF32 */
		if (!write_buffer(info->fd_dumpfile, 0, &ehdr32, sizeof(ehdr32),
		    info->name_dumpfile))
			goto out;
	}

	/*
	 * Pre-calculate the required size to store eraseinfo in ELF note
	 * section so that we can add enough space in ELF notes section and
	 * adjust the PT_LOAD offset accordingly.
	 */
	while (fl_info) {
		struct erase_info *ei;

		if (!fl_info->erase_info_idx)
			continue;
		ei = &erase_info[fl_info->erase_info_idx];
		if (fl_info->nullify)
			sprintf(size_str, "nullify\n");
		else
			sprintf(size_str, "%ld\n", fl_info->size);

		size_eraseinfo += strlen("erase ") +
				strlen(ei->symbol_expr) + 1 +
				strlen(size_str);
		fl_info = fl_info->next;
	}

	/*
	 * Store the size_eraseinfo for later use in write_elf_eraseinfo()
	 * function.
	 */
	info->size_elf_eraseinfo = size_eraseinfo;
	DEBUG_MSG("erase info size: %lu\n", info->size_elf_eraseinfo);

	/*
	 * Write a PT_NOTE header.
	 */
	if (!(phnum = get_phnum_memory()))
		goto out;

	for (i = 0; i < phnum; i++) {
		if (!get_phdr_memory(i, &note))
			return FALSE;
		if (note.p_type == PT_NOTE)
			break;
	}
	if (note.p_type != PT_NOTE) {
		ERRMSG("Can't get a PT_NOTE header.\n");
		goto out;
	}

	if (is_elf64_memory()) { /* ELF64 */
		cd_header->offset    = sizeof(ehdr64);
		offset_note_dumpfile = sizeof(ehdr64)
		    + sizeof(Elf64_Phdr) * ehdr64.e_phnum;
	} else {
		cd_header->offset    = sizeof(ehdr32);
		offset_note_dumpfile = sizeof(ehdr32)
		    + sizeof(Elf32_Phdr) * ehdr32.e_phnum;
	}
	offset_note_memory = note.p_offset;
	note.p_offset      = offset_note_dumpfile;
	size_note          = note.p_filesz;

	/*
	 * Modify the note size in PT_NOTE header to accomodate eraseinfo data.
	 * Eraseinfo will be written later.
	 */
	if (info->size_elf_eraseinfo) {
		if (is_elf64_memory())
			note.p_filesz += sizeof(Elf64_Nhdr);
		else
			note.p_filesz += sizeof(Elf32_Nhdr);
		note.p_filesz += roundup(ERASEINFO_NOTE_NAME_BYTES, 4) +
					roundup(size_eraseinfo, 4);
	}

	if (!write_elf_phdr(cd_header, &note))
		goto out;

	/*
	 * Write a PT_NOTE segment.
	 * PT_LOAD header will be written later.
	 */
	if ((buf = malloc(size_note)) == NULL) {
		ERRMSG("Can't allocate memory for PT_NOTE segment. %s\n",
		    strerror(errno));
		goto out;
	}
	if (lseek(info->fd_memory, offset_note_memory, SEEK_SET) == failed) {
		ERRMSG("Can't seek the dump memory(%s). %s\n",
		    info->name_memory, strerror(errno));
		goto out;
	}
	if (read(info->fd_memory, buf, size_note) != size_note) {
		ERRMSG("Can't read the dump memory(%s). %s\n",
		    info->name_memory, strerror(errno));
		goto out;
	}
	if (!write_buffer(info->fd_dumpfile, offset_note_dumpfile, buf,
	    size_note, info->name_dumpfile))
		goto out;

	/* Set the size_note with new size. */
	size_note          = note.p_filesz;

	/*
	 * Set an offset of PT_LOAD segment.
	 */
	info->offset_load_dumpfile = offset_note_dumpfile + size_note;
	info->offset_note_dumpfile = offset_note_dumpfile;

	ret = TRUE;
out:
	if (buf != NULL)
		free(buf);

	return ret;
}

int
write_kdump_header(void)
{
	int ret = FALSE;
	size_t size;
	off_t offset_note, offset_vmcoreinfo;
	unsigned long size_note, size_vmcoreinfo;
	struct disk_dump_header *dh = info->dump_header;
	struct kdump_sub_header kh;
	char *buf = NULL;

	if (info->flag_elf_dumpfile)
		return FALSE;

	get_pt_note(&offset_note, &size_note);

	/*
	 * Write common header
	 */
	strncpy(dh->signature, KDUMP_SIGNATURE, strlen(KDUMP_SIGNATURE));
	dh->header_version = 5;
  	dh->block_size     = info->page_size;
	dh->sub_hdr_size   = sizeof(kh) + size_note;
	dh->sub_hdr_size   = divideup(dh->sub_hdr_size, dh->block_size);
	dh->max_mapnr      = info->max_mapnr;
	dh->nr_cpus        = get_nr_cpus();
	dh->bitmap_blocks  = divideup(info->len_bitmap, dh->block_size);
	memcpy(&dh->timestamp, &info->timestamp, sizeof(dh->timestamp));
	memcpy(&dh->utsname, &info->system_utsname, sizeof(dh->utsname));

	size = sizeof(struct disk_dump_header);
	if (!write_buffer(info->fd_dumpfile, 0, dh, size, info->name_dumpfile))
		return FALSE;

	/*
	 * Write sub header
	 */
	size = sizeof(struct kdump_sub_header);
	memset(&kh, 0, size);
	kh.phys_base  = info->phys_base;
	kh.dump_level = info->dump_level;
	if (info->flag_split) {
		kh.split = 1;
		kh.start_pfn = info->split_start_pfn;
		kh.end_pfn   = info->split_end_pfn;
	}
	if (has_pt_note()) {
		/*
		 * Write ELF note section
		 */
		kh.offset_note
			= DISKDUMP_HEADER_BLOCKS * dh->block_size + sizeof(kh);
		kh.size_note = size_note;

		buf = malloc(size_note);
		if (buf == NULL) {
			ERRMSG("Can't allocate memory for ELF note section. %s\n",
			    strerror(errno));
			return FALSE;
		}
		if (lseek(info->fd_memory, offset_note, SEEK_SET) < 0) {
			ERRMSG("Can't seek the dump memory(%s). %s\n",
			    info->name_memory, strerror(errno));
			goto out;
		}
		if (read(info->fd_memory, buf, size_note) != size_note) {
			ERRMSG("Can't read the dump memory(%s). %s\n",
			    info->name_memory, strerror(errno));
			goto out;
		}
		if (!write_buffer(info->fd_dumpfile, kh.offset_note, buf,
		    kh.size_note, info->name_dumpfile))
			goto out;

		if (has_vmcoreinfo()) {
			get_vmcoreinfo(&offset_vmcoreinfo, &size_vmcoreinfo);
			/*
			 * Set vmcoreinfo data
			 *
			 * NOTE: ELF note section contains vmcoreinfo data, and
			 *       kh.offset_vmcoreinfo points the vmcoreinfo data.
			 */
			kh.offset_vmcoreinfo
			    = offset_vmcoreinfo - offset_note
			      + kh.offset_note;
			kh.size_vmcoreinfo = size_vmcoreinfo;
		}
	}
	/*
	 * While writing dump data to STDOUT, delay the writing of sub header
	 * untill we gather erase info offset and size.
	 */
	if (!info->flag_flatten) {
		if (!write_buffer(info->fd_dumpfile, dh->block_size, &kh,
		    size, info->name_dumpfile))
			goto out;
	}

	info->sub_header = kh;
	info->offset_bitmap1
	    = (DISKDUMP_HEADER_BLOCKS + dh->sub_hdr_size) * dh->block_size;

	ret = TRUE;
out:
	if (buf)
		free(buf);

	return ret;
}

unsigned long long
get_num_dumpable(void)
{
	unsigned long long pfn, num_dumpable;
	struct dump_bitmap bitmap2;

	initialize_2nd_bitmap(&bitmap2);

	for (pfn = 0, num_dumpable = 0; pfn < info->max_mapnr; pfn++) {
		if (is_dumpable(&bitmap2, pfn))
			num_dumpable++;
	}
	return num_dumpable;
}

void
split_filter_info(struct filter_info *prev, unsigned long long next_paddr,
						size_t size)
{
	struct filter_info *new;

	if ((new = calloc(1, sizeof(struct filter_info))) == NULL) {
		ERRMSG("Can't allocate memory to split filter info\n");
		return;
	}
	new->nullify = prev->nullify;
	new->erase_info_idx = prev->erase_info_idx;
	new->size_idx = prev->size_idx;
	new->paddr = next_paddr;
	new->size = size;
	new->next = prev->next;
	prev->next = new;
}

void
update_erase_info(struct filter_info *fi)
{
	struct erase_info *ei;

	if (!fi->erase_info_idx)
		return;

	ei = &erase_info[fi->erase_info_idx];

	if (!ei->sizes) {
		/* First time, allocate sizes array */
		ei->sizes = calloc(ei->num_sizes, sizeof(long));
		if (!ei->sizes) {
			ERRMSG("Can't allocate memory for erase info sizes\n");
			return;
		}
	}
	ei->erased = 1;
	if (!fi->nullify)
		ei->sizes[fi->size_idx] += fi->size;
	else
		ei->sizes[fi->size_idx] = -1;
}

int
extract_filter_info(unsigned long long start_paddr,
			unsigned long long end_paddr,
			struct filter_info *fl_info)
{
	struct filter_info *fi = filter_info;
	struct filter_info *prev = NULL;
	size_t size1, size2;

	if (!fl_info)
		return FALSE;

	while (fi) {
		if ((fi->paddr >= start_paddr) && (fi->paddr < end_paddr)) {
			size1 = end_paddr - fi->paddr;
			if (fi->size <= size1)
				break;
			size2 = fi->size - size1;
			fi->size = size1;
			split_filter_info(fi, fi->paddr + size1, size2);
			break;
		}
		prev = fi;
		fi = fi->next;
	}
	if (!fi)
		return FALSE;

	*fl_info = *fi;
	fl_info->next = NULL;
	/* Delete this node */
	if (!prev)
		filter_info = fi->next;
	else
		prev->next = fi->next;
	update_erase_info(fi);
	free(fi);
	return TRUE;
}

void
filter_data_buffer(unsigned char *buf, unsigned long long paddr,
					size_t size)
{
	struct filter_info fl_info;
	unsigned char *buf_ptr;

	while (extract_filter_info(paddr, paddr + size, &fl_info)) {
		buf_ptr = buf + (fl_info.paddr - paddr);
		if (fl_info.nullify)
			memset(buf_ptr, 0, fl_info.size);
		else
			memset(buf_ptr, 'X', fl_info.size);
	}
}

int
write_elf_load_segment(struct cache_data *cd_page, unsigned long long paddr,
		       off_t off_memory, long long size)
{
	long page_size = info->page_size;
	long long bufsz_write;
	char buf[info->page_size];

	off_memory = paddr_to_offset2(paddr, off_memory);
	if (!off_memory) {
		ERRMSG("Can't convert physaddr(%llx) to an offset.\n",
		    paddr);
		return FALSE;
	}
	if (lseek(info->fd_memory, off_memory, SEEK_SET) < 0) {
		ERRMSG("Can't seek the dump memory(%s). %s\n",
		    info->name_memory, strerror(errno));
		return FALSE;
	}

	while (size > 0) {
		if (size >= page_size)
			bufsz_write = page_size;
		else
			bufsz_write = size;

		if (read(info->fd_memory, buf, bufsz_write) != bufsz_write) {
			ERRMSG("Can't read the dump memory(%s). %s\n",
			    info->name_memory, strerror(errno));
			return FALSE;
		}
		filter_data_buffer((unsigned char *)buf, paddr, bufsz_write);
		paddr += bufsz_write;
		if (!write_cache(cd_page, buf, bufsz_write))
			return FALSE;

		size -= page_size;
	}
	return TRUE;
}

int
write_elf_pages(struct cache_data *cd_header, struct cache_data *cd_page)
{
	int i, phnum;
	long page_size = info->page_size;
	unsigned long long pfn, pfn_start, pfn_end, paddr, num_excluded;
	unsigned long long num_dumpable, num_dumped = 0, per;
	unsigned long long memsz, filesz;
	unsigned long frac_head, frac_tail;
	off_t off_seg_load, off_memory;
	Elf64_Phdr load;
	struct dump_bitmap bitmap2;
	struct timeval tv_start;

	if (!info->flag_elf_dumpfile)
		return FALSE;

	initialize_2nd_bitmap(&bitmap2);

	num_dumpable = get_num_dumpable();
	per = num_dumpable / 100;

	off_seg_load    = info->offset_load_dumpfile;
	cd_page->offset = info->offset_load_dumpfile;

	if (!(phnum = get_phnum_memory()))
		return FALSE;

	gettimeofday(&tv_start, NULL);

	for (i = 0; i < phnum; i++) {
		if (!get_phdr_memory(i, &load))
			return FALSE;

		if (load.p_type != PT_LOAD)
			continue;

		off_memory= load.p_offset;
		paddr     = load.p_paddr;
		pfn_start = paddr_to_pfn(load.p_paddr);
		pfn_end   = paddr_to_pfn(load.p_paddr + load.p_memsz);
		frac_head = page_size - (load.p_paddr % page_size);
		frac_tail = (load.p_paddr + load.p_memsz)%page_size;

		num_excluded = 0;
		memsz  = 0;
		filesz = 0;
		if (frac_head && (frac_head != page_size)) {
			memsz  = frac_head;
			filesz = frac_head;
			pfn_start++;
		}

		if (frac_tail)
			pfn_end++;

		for (pfn = pfn_start; pfn < pfn_end; pfn++) {
			if (!is_dumpable(&bitmap2, pfn)) {
				num_excluded++;
				if ((pfn == pfn_end - 1) && frac_tail)
					memsz += frac_tail;
				else
					memsz += page_size;
				continue;
			}

			if ((num_dumped % per) == 0)
				print_progress(PROGRESS_COPY, num_dumped, num_dumpable);

			num_dumped++;

			/*
			 * The dumpable pages are continuous.
			 */
			if (!num_excluded) {
				if ((pfn == pfn_end - 1) && frac_tail) {
					memsz  += frac_tail;
					filesz += frac_tail;
				} else {
					memsz  += page_size;
					filesz += page_size;
				}
				continue;
			/*
			 * If the number of the contiguous pages to be excluded
			 * is 255 or less, those pages are not excluded.
			 */
			} else if (num_excluded < PFN_EXCLUDED) {
				if ((pfn == pfn_end - 1) && frac_tail) {
					memsz  += frac_tail;
					filesz += (page_size*num_excluded
					    + frac_tail);
				}else {
					memsz  += page_size;
					filesz += (page_size*num_excluded
					    + page_size);
				}
				num_excluded = 0;
				continue;
			}

			/*
			 * If the number of the contiguous pages to be excluded
			 * is 256 or more, those pages are excluded really.
			 * And a new PT_LOAD segment is created.
			 */
			load.p_memsz  = memsz;
			load.p_filesz = filesz;
			if (load.p_filesz)
				load.p_offset = off_seg_load;
			else
				/*
				 * If PT_LOAD segment does not have real data
				 * due to the all excluded pages, the file
				 * offset is not effective and it should be 0.
				 */
				load.p_offset = 0;

			/*
			 * Write a PT_LOAD header.
			 */
			if (!write_elf_phdr(cd_header, &load))
				return FALSE;

			/*
			 * Write a PT_LOAD segment.
			 */
			if (load.p_filesz)
				if (!write_elf_load_segment(cd_page, paddr,
				    off_memory, load.p_filesz))
					return FALSE;

			load.p_paddr += load.p_memsz;
#ifdef __x86__
			/*
			 * FIXME:
			 *  (x86) Fill PT_LOAD headers with appropriate
			 *        virtual addresses.
			 */
			if (load.p_paddr < MAXMEM)
				load.p_vaddr += load.p_memsz;
#else
			load.p_vaddr += load.p_memsz;
#endif /* x86 */
			paddr  = load.p_paddr;
			off_seg_load += load.p_filesz;

			num_excluded = 0;
			memsz  = page_size;
			filesz = page_size;
		}
		/*
		 * Write the last PT_LOAD.
		 */
		load.p_memsz  = memsz;
		load.p_filesz = filesz;
		load.p_offset = off_seg_load;

		/*
		 * Write a PT_LOAD header.
		 */
		if (!write_elf_phdr(cd_header, &load))
			return FALSE;

		/*
		 * Write a PT_LOAD segment.
		 */
		if (load.p_filesz)
			if (!write_elf_load_segment(cd_page, paddr,
						    off_memory, load.p_filesz))
				return FALSE;

		off_seg_load += load.p_filesz;
	}
	if (!write_cache_bufsz(cd_header))
		return FALSE;
	if (!write_cache_bufsz(cd_page))
		return FALSE;

	/*
	 * print [100 %]
	 */
	print_progress(PROGRESS_COPY, num_dumpable, num_dumpable);
	print_execution_time(PROGRESS_COPY, &tv_start);
	PROGRESS_MSG("\n");

	return TRUE;
}

/*
 * This function is specific for reading page.
 *
 * If reading the separated page on different PT_LOAD segments,
 * this function gets the page data from both segments. This is
 * worthy of ia64 /proc/vmcore. In ia64 /proc/vmcore, region 5
 * segment is overlapping to region 7 segment. The following is
 * example (page_size is 16KBytes):
 *
 *  region |       paddr        |       memsz
 * --------+--------------------+--------------------
 *     5   | 0x0000000004000000 | 0x0000000000638ce0
 *     7   | 0x0000000004000000 | 0x0000000000db3000
 *
 * In the above example, the last page of region 5 is 0x4638000
 * and the segment does not contain complete data of this page.
 * Then this function gets the data of 0x4638000 - 0x4638ce0
 * from region 5, and gets the remaining data from region 7.
 */
int
read_pfn(unsigned long long pfn, unsigned char *buf)
{
	unsigned long long paddr;
	off_t offset1, offset2;
	size_t size1, size2;

	paddr = pfn_to_paddr(pfn);
	if (info->flag_refiltering) {
		if (!readmem(PADDR, paddr, buf, info->page_size)) {
			ERRMSG("Can't get the page data.\n");
			return FALSE;
		}
		filter_data_buffer(buf, paddr, info->page_size);
		return TRUE;
	}

	offset1 = paddr_to_offset(paddr);
	offset2 = paddr_to_offset(paddr + info->page_size);

	/*
	 * Check the separated page on different PT_LOAD segments.
	 */
	if (offset1 + info->page_size == offset2) {
		size1 = info->page_size;
	} else {
		for (size1 = 1; size1 < info->page_size; size1++) {
			offset2 = paddr_to_offset(paddr + size1);
			if (offset1 + size1 != offset2)
				break;
		}
	}
	if (!readmem(PADDR, paddr, buf, size1)) {
		ERRMSG("Can't get the page data.\n");
		return FALSE;
	}
	filter_data_buffer(buf, paddr, size1);
	if (size1 != info->page_size) {
		size2 = info->page_size - size1;
		if (!offset2) {
			memset(buf + size1, 0, size2);
		} else {
			if (!readmem(PADDR, paddr + size1, buf + size1, size2)) {
				ERRMSG("Can't get the page data.\n");
				return FALSE;
			}
			filter_data_buffer(buf + size1, paddr + size1, size2);
		}
	}
	return TRUE;
}

int
write_kdump_pages(struct cache_data *cd_header, struct cache_data *cd_page)
{
 	unsigned long long pfn, per, num_dumpable, num_dumped = 0;
	unsigned long long start_pfn, end_pfn;
	unsigned long size_out;
	struct page_desc pd, pd_zero;
	off_t offset_data = 0;
	struct disk_dump_header *dh = info->dump_header;
	unsigned char buf[info->page_size], *buf_out = NULL;
	unsigned long len_buf_out;
	struct dump_bitmap bitmap2;
	struct timeval tv_start;
	const off_t failed = (off_t)-1;

	int ret = FALSE;

	if (info->flag_elf_dumpfile)
		return FALSE;

	initialize_2nd_bitmap(&bitmap2);

	len_buf_out = compressBound(info->page_size);
	if ((buf_out = malloc(len_buf_out)) == NULL) {
		ERRMSG("Can't allocate memory for the compression buffer. %s\n",
		    strerror(errno));
		goto out;
	}

	num_dumpable = get_num_dumpable();
	per = num_dumpable / 100;

	/*
	 * Calculate the offset of the page data.
	 */
	cd_header->offset
	    = (DISKDUMP_HEADER_BLOCKS + dh->sub_hdr_size + dh->bitmap_blocks)
		* dh->block_size;
	cd_page->offset = cd_header->offset + sizeof(page_desc_t)*num_dumpable;
	offset_data  = cd_page->offset;

	/*
	 * Set a fileoffset of Physical Address 0x0.
	 */
	if (lseek(info->fd_memory, get_offset_pt_load_memory(), SEEK_SET)
	    == failed) {
		ERRMSG("Can't seek the dump memory(%s). %s\n",
		    info->name_memory, strerror(errno));
		goto out;
	}

	/*
	 * Write the data of zero-filled page.
	 */
	gettimeofday(&tv_start, NULL);
	if (info->dump_level & DL_EXCLUDE_ZERO) {
		pd_zero.size = info->page_size;
		pd_zero.flags = 0;
		pd_zero.offset = offset_data;
		pd_zero.page_flags = 0;
		memset(buf, 0, pd_zero.size);
		if (!write_cache(cd_page, buf, pd_zero.size))
			goto out;
		offset_data  += pd_zero.size;
	}
	if (info->flag_split) {
		start_pfn = info->split_start_pfn;
		end_pfn   = info->split_end_pfn;
	}
	else {
		start_pfn = 0;
		end_pfn   = info->max_mapnr;
	}

	for (pfn = start_pfn; pfn < end_pfn; pfn++) {

		if ((num_dumped % per) == 0)
			print_progress(PROGRESS_COPY, num_dumped, num_dumpable);

		/*
		 * Check the excluded page.
		 */
		if (!is_dumpable(&bitmap2, pfn))
			continue;

		num_dumped++;

		if (!read_pfn(pfn, buf))
			goto out;

		/*
		 * Exclude the page filled with zeros.
		 */
		if ((info->dump_level & DL_EXCLUDE_ZERO)
		    && is_zero_page(buf, info->page_size)) {
			if (!write_cache(cd_header, &pd_zero, sizeof(page_desc_t)))
				goto out;
			pfn_zero++;
			continue;
		}
		/*
		 * Compress the page data.
		 */
		size_out = len_buf_out;
		if (info->flag_compress
		    && (compress2(buf_out, &size_out, buf,
		    info->page_size, Z_BEST_SPEED) == Z_OK)
		    && (size_out < info->page_size)) {
			pd.flags = 1;
			pd.size  = size_out;
			memcpy(buf, buf_out, pd.size);
		} else {
			pd.flags = 0;
			pd.size  = info->page_size;
		}
		pd.page_flags = 0;
		pd.offset     = offset_data;
		offset_data  += pd.size;

		/*
		 * Write the page header.
		 */
		if (!write_cache(cd_header, &pd, sizeof(page_desc_t)))
			goto out;

		/*
		 * Write the page data.
		 */
		if (!write_cache(cd_page, buf, pd.size))
			goto out;
	}

	/*
	 * Write the remainder.
	 */
	if (!write_cache_bufsz(cd_page))
		goto out;
	if (!write_cache_bufsz(cd_header))
		goto out;

	/*
	 * print [100 %]
	 */
	print_progress(PROGRESS_COPY, num_dumpable, num_dumpable);
	print_execution_time(PROGRESS_COPY, &tv_start);
	PROGRESS_MSG("\n");

	ret = TRUE;
out:
	if (buf_out != NULL)
		free(buf_out);

	return ret;
}

/*
 * Copy eraseinfo from input dumpfile/vmcore to output dumpfile.
 */
static int
copy_eraseinfo(struct cache_data *cd_eraseinfo)
{
	char *buf = NULL;
	off_t offset;
	unsigned long size;
	int ret = FALSE;

	get_eraseinfo(&offset, &size);
	buf = malloc(size);
	if (buf == NULL) {
		ERRMSG("Can't allocate memory for erase info section. %s\n",
		    strerror(errno));
		return FALSE;
	}
	if (lseek(info->fd_memory, offset, SEEK_SET) < 0) {
		ERRMSG("Can't seek the dump memory(%s). %s\n",
		    info->name_memory, strerror(errno));
		goto out;
	}
	if (read(info->fd_memory, buf, size) != size) {
		ERRMSG("Can't read the dump memory(%s). %s\n",
		    info->name_memory, strerror(errno));
		goto out;
	}
	if (!write_cache(cd_eraseinfo, buf, size))
		goto out;
	ret = TRUE;
out:
	if (buf)
		free(buf);
	return ret;
}

static int
update_eraseinfo_of_sub_header(off_t offset_eraseinfo,
			       unsigned long size_eraseinfo)
{
	off_t offset;

	/* seek to kdump sub header offset */
	offset = DISKDUMP_HEADER_BLOCKS * info->page_size;

	info->sub_header.offset_eraseinfo = offset_eraseinfo;
	info->sub_header.size_eraseinfo   = size_eraseinfo;

	if (!write_buffer(info->fd_dumpfile, offset, &info->sub_header,
			sizeof(struct kdump_sub_header), info->name_dumpfile))
		return FALSE;

	return TRUE;
}

/*
 * Traverse through eraseinfo nodes and write it to the o/p dumpfile if the
 * node has erased flag set.
 */
int
write_eraseinfo(struct cache_data *cd_page, unsigned long *size_out)
{
	int i, j, obuf_size = 0, ei_size = 0;
	int ret = FALSE;
	unsigned long size_eraseinfo = 0;
	char *obuf = NULL;
	char size_str[MAX_SIZE_STR_LEN];

	for (i = 1; i < num_erase_info; i++) {
		if (!erase_info[i].erased)
			continue;
		for (j = 0; j < erase_info[i].num_sizes; j++) {
			if (erase_info[i].sizes[j] > 0)
				sprintf(size_str, "%ld\n",
						erase_info[i].sizes[j]);
			else if (erase_info[i].sizes[j] == -1)
				sprintf(size_str, "nullify\n");

			/* Calculate the required buffer size. */
			ei_size = strlen("erase ") +
					strlen(erase_info[i].symbol_expr) + 1 +
					strlen(size_str) +
					1;
			/*
			 * If obuf is allocated in the previous run and is
			 * big enough to hold current erase info string then
			 * reuse it otherwise realloc.
			 */
			if (ei_size > obuf_size) {
				obuf_size = ei_size;
				obuf = realloc(obuf, obuf_size);
				if (!obuf) {
					ERRMSG("Can't allocate memory for"
						" output buffer\n");
					return FALSE;
				}
			}
			sprintf(obuf, "erase %s %s", erase_info[i].symbol_expr,
							size_str);
			DEBUG_MSG(obuf);
			if (!write_cache(cd_page, obuf, strlen(obuf)))
				goto out;
			size_eraseinfo += strlen(obuf);
		}
	}
	/*
	 * Write the remainder.
	 */
	if (!write_cache_bufsz(cd_page))
		goto out;

	*size_out = size_eraseinfo;
	ret = TRUE;
out:
	if (obuf)
		free(obuf);

	return ret;
}

int
write_elf_eraseinfo(struct cache_data *cd_header)
{
	char note[MAX_SIZE_NHDR];
	char buf[ERASEINFO_NOTE_NAME_BYTES + 4];
	off_t offset_eraseinfo;
	unsigned long note_header_size, size_written, size_note;

	if (!info->size_elf_eraseinfo)
		return TRUE;

	DEBUG_MSG("Writing erase info...\n");

	/* calculate the eraseinfo ELF note offset */
	get_pt_note(NULL, &size_note);
	cd_header->offset = info->offset_note_dumpfile +
				roundup(size_note, 4);

	/* Write eraseinfo ELF note header. */
	memset(note, 0, sizeof(note));
	if (is_elf64_memory()) {
		Elf64_Nhdr *nh = (Elf64_Nhdr *)note;

		note_header_size = sizeof(Elf64_Nhdr);
		nh->n_namesz = ERASEINFO_NOTE_NAME_BYTES;
		nh->n_descsz = info->size_elf_eraseinfo;
		nh->n_type = 0;
	} else {
		Elf32_Nhdr *nh = (Elf32_Nhdr *)note;

		note_header_size = sizeof(Elf32_Nhdr);
		nh->n_namesz = ERASEINFO_NOTE_NAME_BYTES;
		nh->n_descsz = info->size_elf_eraseinfo;
		nh->n_type = 0;
	}
	if (!write_cache(cd_header, note, note_header_size))
		return FALSE;

	/* Write eraseinfo Note name */
	memset(buf, 0, sizeof(buf));
	memcpy(buf, ERASEINFO_NOTE_NAME, ERASEINFO_NOTE_NAME_BYTES);
	if (!write_cache(cd_header, buf,
				roundup(ERASEINFO_NOTE_NAME_BYTES, 4)))
		return FALSE;

	offset_eraseinfo = cd_header->offset;
	if (!write_eraseinfo(cd_header, &size_written))
		return FALSE;

	/*
	 * The actual eraseinfo written may be less than pre-calculated size.
	 * Hence fill up the rest of size with zero's.
	 */
	if (size_written < info->size_elf_eraseinfo)
		write_cache_zero(cd_header,
				info->size_elf_eraseinfo - size_written);

	DEBUG_MSG("offset_eraseinfo: %llx, size_eraseinfo: %ld\n",
		(unsigned long long)offset_eraseinfo, info->size_elf_eraseinfo);

	return TRUE;
}

int
write_kdump_eraseinfo(struct cache_data *cd_page)
{
	off_t offset_eraseinfo;
	unsigned long size_eraseinfo, size_written;

	DEBUG_MSG("Writing erase info...\n");
	offset_eraseinfo = cd_page->offset;

	/*
	 * In case of refiltering copy the existing eraseinfo from input
	 * dumpfile to o/p dumpfile.
	 */
	if (has_eraseinfo()) {
		get_eraseinfo(NULL, &size_eraseinfo);
		if (!copy_eraseinfo(cd_page))
			return FALSE;
	} else
		size_eraseinfo = 0;

	if (!write_eraseinfo(cd_page, &size_written))
		return FALSE;

	size_eraseinfo += size_written;
	DEBUG_MSG("offset_eraseinfo: %llx, size_eraseinfo: %ld\n",
		(unsigned long long)offset_eraseinfo, size_eraseinfo);

	if (size_eraseinfo)
		/* Update the erase info offset and size in kdump sub header */
		if (!update_eraseinfo_of_sub_header(offset_eraseinfo,
						    size_eraseinfo))
			return FALSE;

	return TRUE;
}

int
write_kdump_bitmap(void)
{
	struct cache_data bm;
	long buf_size;
	off_t offset;

	int ret = FALSE;

	if (info->flag_elf_dumpfile)
		return FALSE;

	bm.fd        = info->fd_bitmap;
	bm.file_name = info->name_bitmap;
	bm.offset    = 0;
	bm.buf       = NULL;

	if ((bm.buf = calloc(1, BUFSIZE_BITMAP)) == NULL) {
		ERRMSG("Can't allocate memory for dump bitmap buffer. %s\n",
		    strerror(errno));
		goto out;
	}
	offset = info->offset_bitmap1;
	buf_size = info->len_bitmap;

	while (buf_size > 0) {
		if (buf_size >= BUFSIZE_BITMAP)
			bm.cache_size = BUFSIZE_BITMAP;
		else
			bm.cache_size = buf_size;

		if(!read_cache(&bm))
			goto out;

		if (!write_buffer(info->fd_dumpfile, offset,
		    bm.buf, bm.cache_size, info->name_dumpfile))
			goto out;

		offset += bm.cache_size;
		buf_size -= BUFSIZE_BITMAP;
	}
	ret = TRUE;
out:
	if (bm.buf != NULL)
		free(bm.buf);

	return ret;
}

void
close_vmcoreinfo(void)
{
	if(fclose(info->file_vmcoreinfo) < 0)
		ERRMSG("Can't close the vmcoreinfo file(%s). %s\n",
		    info->name_vmcoreinfo, strerror(errno));
}

void
close_dump_memory(void)
{
	if ((info->fd_memory = close(info->fd_memory)) < 0)
		ERRMSG("Can't close the dump memory(%s). %s\n",
		    info->name_memory, strerror(errno));
}

void
close_dump_file(void)
{
	if (info->flag_flatten)
		return;

	if ((info->fd_dumpfile = close(info->fd_dumpfile)) < 0)
		ERRMSG("Can't close the dump file(%s). %s\n",
		    info->name_dumpfile, strerror(errno));
}

void
close_dump_bitmap(void)
{
	if ((info->fd_bitmap = close(info->fd_bitmap)) < 0)
		ERRMSG("Can't close the bitmap file(%s). %s\n",
		    info->name_bitmap, strerror(errno));
	free(info->name_bitmap);
	info->name_bitmap = NULL;
}

void
close_kernel_file(void)
{
	if (info->name_vmlinux) {
		if ((info->fd_vmlinux = close(info->fd_vmlinux)) < 0) {
			ERRMSG("Can't close the kernel file(%s). %s\n",
			    info->name_vmlinux, strerror(errno));
		}
	}
	if (info->name_xen_syms) {
		if ((info->fd_xen_syms = close(info->fd_xen_syms)) < 0) {
			ERRMSG("Can't close the kernel file(%s). %s\n",
			    info->name_xen_syms, strerror(errno));
		}
	}
}

/*
 * Close the following files when it generates the vmcoreinfo file.
 * - vmlinux
 * - vmcoreinfo file
 */
int
close_files_for_generating_vmcoreinfo(void)
{
	close_kernel_file();

	close_vmcoreinfo();

	return TRUE;
}

/*
 * Close the following file when it rearranges the dump data.
 * - dump file
 */
int
close_files_for_rearranging_dumpdata(void)
{
	close_dump_file();

	return TRUE;
}

/*
 * Close the following files when it creates the dump file.
 * - dump mem
 * - dump file
 * - bit map
 * if it reads the vmcoreinfo file
 *   - vmcoreinfo file
 * else
 *   - vmlinux
 */
int
close_files_for_creating_dumpfile(void)
{
	if (info->max_dump_level > DL_EXCLUDE_ZERO)
		close_kernel_file();

	/* free name for vmcoreinfo */
	if (has_vmcoreinfo()) {
		free(info->name_vmcoreinfo);
		info->name_vmcoreinfo = NULL;
	}
	close_dump_memory();

	close_dump_bitmap();

	return TRUE;
}

/*
 * for Xen extraction
 */
int
get_symbol_info_xen(void)
{
	/*
	 * Common symbol
	 */
	SYMBOL_INIT(dom_xen, "dom_xen");
	SYMBOL_INIT(dom_io, "dom_io");
	SYMBOL_INIT(domain_list, "domain_list");
	SYMBOL_INIT(frame_table, "frame_table");
	SYMBOL_INIT(alloc_bitmap, "alloc_bitmap");
	SYMBOL_INIT(max_page, "max_page");
	SYMBOL_INIT(xenheap_phys_end, "xenheap_phys_end");

	/*
	 * Architecture specific
	 */
	SYMBOL_INIT(pgd_l2, "idle_pg_table_l2");	/* x86 */
	SYMBOL_INIT(pgd_l3, "idle_pg_table_l3");	/* x86-PAE */
	if (SYMBOL(pgd_l3) == NOT_FOUND_SYMBOL)
		SYMBOL_INIT(pgd_l3, "idle_pg_table");	/* x86-PAE */
	SYMBOL_INIT(pgd_l4, "idle_pg_table_4");		/* x86_64 */
	if (SYMBOL(pgd_l4) == NOT_FOUND_SYMBOL)
		SYMBOL_INIT(pgd_l4, "idle_pg_table");		/* x86_64 */

	SYMBOL_INIT(xen_heap_start, "xen_heap_start");	/* ia64 */
	SYMBOL_INIT(xen_pstart, "xen_pstart");		/* ia64 */
	SYMBOL_INIT(frametable_pg_dir, "frametable_pg_dir");	/* ia64 */

	return TRUE;
}

int
get_structure_info_xen(void)
{
	SIZE_INIT(page_info, "page_info");
	OFFSET_INIT(page_info.count_info, "page_info", "count_info");
	/*
	 * _domain is the first member of union u
	 */
	OFFSET_INIT(page_info._domain, "page_info", "u");

	SIZE_INIT(domain, "domain");
	OFFSET_INIT(domain.domain_id, "domain", "domain_id");
	OFFSET_INIT(domain.next_in_list, "domain", "next_in_list");

	return TRUE;
}

int
get_xen_phys_start(void)
{
	off_t offset, offset_xen_crash_info;
	unsigned long xen_phys_start, size_xen_crash_info;
	const off_t failed = (off_t)-1;

	if (info->xen_phys_start)
		return TRUE;

	get_xen_crash_info(&offset_xen_crash_info, &size_xen_crash_info);
	if (size_xen_crash_info >= SIZE_XEN_CRASH_INFO_V2) {
		offset = offset_xen_crash_info + size_xen_crash_info
			 - sizeof(unsigned long) * 2;
		if (lseek(info->fd_memory, offset, SEEK_SET) == failed) {
			ERRMSG("Can't seek the dump memory(%s). %s\n",
			    info->name_memory, strerror(errno));
			return FALSE;
		}
		if (read(info->fd_memory, &xen_phys_start, sizeof(unsigned long))
		    != sizeof(unsigned long)) {
			ERRMSG("Can't read the dump memory(%s). %s\n",
			    info->name_memory, strerror(errno));
			return FALSE;
		}
		info->xen_phys_start = xen_phys_start;
	}

	return TRUE;
}

int
get_xen_info(void)
{
	unsigned long domain;
	unsigned int domain_id;
	int num_domain;

	if (SYMBOL(alloc_bitmap) == NOT_FOUND_SYMBOL) {
		ERRMSG("Can't get the symbol of alloc_bitmap.\n");
		return FALSE;
	}
	if (!readmem(VADDR_XEN, SYMBOL(alloc_bitmap), &info->alloc_bitmap,
	      sizeof(info->alloc_bitmap))) {
		ERRMSG("Can't get the value of alloc_bitmap.\n");
		return FALSE;
	}
	if (SYMBOL(max_page) == NOT_FOUND_SYMBOL) {
		ERRMSG("Can't get the symbol of max_page.\n");
		return FALSE;
	}
	if (!readmem(VADDR_XEN, SYMBOL(max_page), &info->max_page,
	    sizeof(info->max_page))) {
		ERRMSG("Can't get the value of max_page.\n");
		return FALSE;
	}

	/*
	 * Walk through domain_list
	 */
	if (SYMBOL(domain_list) == NOT_FOUND_SYMBOL) {
		ERRMSG("Can't get the symbol of domain_list.\n");
		return FALSE;
	}
	if (!readmem(VADDR_XEN, SYMBOL(domain_list), &domain, sizeof(domain))){
		ERRMSG("Can't get the value of domain_list.\n");
		return FALSE;
	}

	/*
	 * Get numbers of domain first
	 */
	num_domain = 0;
	while (domain) {
		num_domain++;
		if (!readmem(VADDR_XEN, domain + OFFSET(domain.next_in_list),
		    &domain, sizeof(domain))) {
			ERRMSG("Can't get through the domain_list.\n");
			return FALSE;
		}
	}

	if ((info->domain_list = (struct domain_list *)
	      malloc(sizeof(struct domain_list) * (num_domain + 2))) == NULL) {
		ERRMSG("Can't allcate memory for domain_list.\n");
		return FALSE;
	}

	info->num_domain = num_domain + 2;

	if (!readmem(VADDR_XEN, SYMBOL(domain_list), &domain, sizeof(domain))) {
		ERRMSG("Can't get the value of domain_list.\n");
		return FALSE;
	}
	num_domain = 0;
	while (domain) {
		if (!readmem(VADDR_XEN, domain + OFFSET(domain.domain_id),
		      &domain_id, sizeof(domain_id))) {
			ERRMSG("Can't get the domain_id.\n");
			return FALSE;
		}
		info->domain_list[num_domain].domain_addr = domain;
		info->domain_list[num_domain].domain_id = domain_id;
		/*
		 * pickled_id is set by architecture specific
		 */
		num_domain++;

		if (!readmem(VADDR_XEN, domain + OFFSET(domain.next_in_list),
		     &domain, sizeof(domain))) {
			ERRMSG("Can't get through the domain_list.\n");
			return FALSE;
		}
	}

	/*
	 * special domains
	 */
	if (SYMBOL(dom_xen) == NOT_FOUND_SYMBOL) {
		ERRMSG("Can't get the symbol of dom_xen.\n");
		return FALSE;
	}
	if (!readmem(VADDR_XEN, SYMBOL(dom_xen), &domain, sizeof(domain))) {
		ERRMSG("Can't get the value of dom_xen.\n");
		return FALSE;
	}
	if (!readmem(VADDR_XEN, domain + OFFSET(domain.domain_id), &domain_id,
	    sizeof(domain_id))) {
		ERRMSG( "Can't get the value of dom_xen domain_id.\n");
		return FALSE;
	}
	info->domain_list[num_domain].domain_addr = domain;
	info->domain_list[num_domain].domain_id = domain_id;
	num_domain++;

	if (SYMBOL(dom_io) == NOT_FOUND_SYMBOL) {
		ERRMSG("Can't get the symbol of dom_io.\n");
		return FALSE;
	}
	if (!readmem(VADDR_XEN, SYMBOL(dom_io), &domain, sizeof(domain))) {
		ERRMSG("Can't get the value of dom_io.\n");
		return FALSE;
	}
	if (!readmem(VADDR_XEN, domain + OFFSET(domain.domain_id), &domain_id,
	    sizeof(domain_id))) {
		ERRMSG( "Can't get the value of dom_io domain_id.\n");
		return FALSE;
	}
	info->domain_list[num_domain].domain_addr = domain;
	info->domain_list[num_domain].domain_id = domain_id;

	/*
	 * Get architecture specific data
	 */
	if (!get_xen_info_arch())
		return FALSE;

	return TRUE;
}

void
show_data_xen(void)
{
	int i;

	/*
	 * Show data for debug
	 */
	MSG("\n");
	MSG("SYMBOL(dom_xen): %llx\n", SYMBOL(dom_xen));
	MSG("SYMBOL(dom_io): %llx\n", SYMBOL(dom_io));
	MSG("SYMBOL(domain_list): %llx\n", SYMBOL(domain_list));
	MSG("SYMBOL(xen_heap_start): %llx\n", SYMBOL(xen_heap_start));
	MSG("SYMBOL(frame_table): %llx\n", SYMBOL(frame_table));
	MSG("SYMBOL(alloc_bitmap): %llx\n", SYMBOL(alloc_bitmap));
	MSG("SYMBOL(max_page): %llx\n", SYMBOL(max_page));
	MSG("SYMBOL(pgd_l2): %llx\n", SYMBOL(pgd_l2));
	MSG("SYMBOL(pgd_l3): %llx\n", SYMBOL(pgd_l3));
	MSG("SYMBOL(pgd_l4): %llx\n", SYMBOL(pgd_l4));
	MSG("SYMBOL(xenheap_phys_end): %llx\n", SYMBOL(xenheap_phys_end));
	MSG("SYMBOL(xen_pstart): %llx\n", SYMBOL(xen_pstart));
	MSG("SYMBOL(frametable_pg_dir): %llx\n", SYMBOL(frametable_pg_dir));

	MSG("SIZE(page_info): %ld\n", SIZE(page_info));
	MSG("OFFSET(page_info.count_info): %ld\n", OFFSET(page_info.count_info));
	MSG("OFFSET(page_info._domain): %ld\n", OFFSET(page_info._domain));
	MSG("SIZE(domain): %ld\n", SIZE(domain));
	MSG("OFFSET(domain.domain_id): %ld\n", OFFSET(domain.domain_id));
	MSG("OFFSET(domain.next_in_list): %ld\n", OFFSET(domain.next_in_list));

	MSG("\n");
	MSG("xen_phys_start: %lx\n", info->xen_phys_start);
	MSG("frame_table_vaddr: %lx\n", info->frame_table_vaddr);
	MSG("xen_heap_start: %lx\n", info->xen_heap_start);
	MSG("xen_heap_end:%lx\n", info->xen_heap_end);
	MSG("alloc_bitmap: %lx\n", info->alloc_bitmap);
	MSG("max_page: %lx\n", info->max_page);
	MSG("num_domain: %d\n", info->num_domain);
	for (i = 0; i < info->num_domain; i++) {
		MSG(" %u: %x: %lx\n", info->domain_list[i].domain_id,
			info->domain_list[i].pickled_id,
			info->domain_list[i].domain_addr);
	}
}

int
generate_vmcoreinfo_xen(void)
{
	if ((info->page_size = sysconf(_SC_PAGE_SIZE)) <= 0) {
		ERRMSG("Can't get the size of page.\n");
		return FALSE;
	}
	set_dwarf_debuginfo("xen-syms", NULL,
			    info->name_xen_syms, info->fd_xen_syms);

	if (!get_symbol_info_xen())
		return FALSE;

	if (!get_structure_info_xen())
		return FALSE;

	/*
	 * write 1st kernel's PAGESIZE
	 */
	fprintf(info->file_vmcoreinfo, "%s%ld\n", STR_PAGESIZE,
	    info->page_size);

	/*
	 * write the symbol of 1st kernel
	 */
	WRITE_SYMBOL("dom_xen", dom_xen);
	WRITE_SYMBOL("dom_io", dom_io);
	WRITE_SYMBOL("domain_list", domain_list);
	WRITE_SYMBOL("xen_heap_start", xen_heap_start);
	WRITE_SYMBOL("frame_table", frame_table);
	WRITE_SYMBOL("alloc_bitmap", alloc_bitmap);
	WRITE_SYMBOL("max_page", max_page);
	WRITE_SYMBOL("pgd_l2", pgd_l2);
	WRITE_SYMBOL("pgd_l3", pgd_l3);
	WRITE_SYMBOL("pgd_l4", pgd_l4);
	WRITE_SYMBOL("xenheap_phys_end", xenheap_phys_end);
	WRITE_SYMBOL("xen_pstart", xen_pstart);
	WRITE_SYMBOL("frametable_pg_dir", frametable_pg_dir);

	/*
	 * write the structure size of 1st kernel
	 */
	WRITE_STRUCTURE_SIZE("page_info", page_info);
	WRITE_STRUCTURE_SIZE("domain", domain);

	/*
	 * write the member offset of 1st kernel
	 */
	WRITE_MEMBER_OFFSET("page_info.count_info", page_info.count_info);
	WRITE_MEMBER_OFFSET("page_info._domain", page_info._domain);
	WRITE_MEMBER_OFFSET("domain.domain_id", domain.domain_id);
	WRITE_MEMBER_OFFSET("domain.next_in_list", domain.next_in_list);

	return TRUE;
}

int
read_vmcoreinfo_basic_info_xen(void)
{
	long page_size = FALSE;
	char buf[BUFSIZE_FGETS], *endp;
	unsigned int i;

	if (fseek(info->file_vmcoreinfo, 0, SEEK_SET) < 0) {
		ERRMSG("Can't seek the vmcoreinfo file(%s). %s\n",
		    info->name_vmcoreinfo, strerror(errno));
		return FALSE;
	}

	while (fgets(buf, BUFSIZE_FGETS, info->file_vmcoreinfo)) {
		i = strlen(buf);
		if (!i)
			break;
		if (buf[i - 1] == '\n')
			buf[i - 1] = '\0';
		if (strncmp(buf, STR_PAGESIZE, strlen(STR_PAGESIZE)) == 0) {
			page_size = strtol(buf+strlen(STR_PAGESIZE),&endp,10);
			if ((!page_size || page_size == LONG_MAX)
			    || strlen(endp) != 0) {
				ERRMSG("Invalid data in %s: %s",
				    info->name_vmcoreinfo, buf);
				return FALSE;
			}
			if (!set_page_size(page_size)) {
				ERRMSG("Invalid data in %s: %s",
				    info->name_vmcoreinfo, buf);
				return FALSE;
			}
			break;
		}
	}
	if (!info->page_size) {
		ERRMSG("Invalid format in %s", info->name_vmcoreinfo);
		return FALSE;
	}
	return TRUE;
}

int
read_vmcoreinfo_xen(void)
{
	if (!read_vmcoreinfo_basic_info_xen())
		return FALSE;

	READ_SYMBOL("dom_xen", dom_xen);
	READ_SYMBOL("dom_io", dom_io);
	READ_SYMBOL("domain_list", domain_list);
	READ_SYMBOL("xen_heap_start", xen_heap_start);
	READ_SYMBOL("frame_table", frame_table);
	READ_SYMBOL("alloc_bitmap", alloc_bitmap);
	READ_SYMBOL("max_page", max_page);
	READ_SYMBOL("pgd_l2", pgd_l2);
	READ_SYMBOL("pgd_l3", pgd_l3);
	READ_SYMBOL("pgd_l4", pgd_l4);
	READ_SYMBOL("xenheap_phys_end", xenheap_phys_end);
	READ_SYMBOL("xen_pstart", xen_pstart);
	READ_SYMBOL("frametable_pg_dir", frametable_pg_dir);

	READ_STRUCTURE_SIZE("page_info", page_info);
	READ_STRUCTURE_SIZE("domain", domain);

	READ_MEMBER_OFFSET("page_info.count_info", page_info.count_info);
	READ_MEMBER_OFFSET("page_info._domain", page_info._domain);
	READ_MEMBER_OFFSET("domain.domain_id", domain.domain_id);
	READ_MEMBER_OFFSET("domain.next_in_list", domain.next_in_list);

	return TRUE;
}

int
allocated_in_map(unsigned long long pfn)
{
	static unsigned long long cur_idx = -1;
	static unsigned long cur_word;
	unsigned long long idx;

	idx = pfn / PAGES_PER_MAPWORD;
	if (idx != cur_idx) {
		if (!readmem(VADDR_XEN,
		    info->alloc_bitmap + idx * sizeof(unsigned long),
		    &cur_word, sizeof(cur_word))) {
			ERRMSG("Can't access alloc_bitmap.\n");
			return 0;
		}
		cur_idx = idx;
	}

	return !!(cur_word & (1UL << (pfn & (PAGES_PER_MAPWORD - 1))));
}

int
is_select_domain(unsigned int id)
{
	int i;

	/* selected domain is fix to dom0 only now !!
	   (yes... domain_list is not necessary right now,
		   it can get from "dom0" directly) */

	for (i = 0; i < info->num_domain; i++) {
		if (info->domain_list[i].domain_id == 0 &&
		    info->domain_list[i].pickled_id == id)
			return TRUE;
	}

	return FALSE;
}

int
exclude_xen_user_domain(void)
{
	int i;
	unsigned int count_info, _domain;
	unsigned int num_pt_loads = get_num_pt_loads();
	unsigned long page_info_addr;
	unsigned long long phys_start, phys_end;
	unsigned long long pfn, pfn_end;
	unsigned long long j, size;
	struct timeval tv_start;

	gettimeofday(&tv_start, NULL);

	/*
	 * NOTE: the first half of bitmap is not used for Xen extraction
	 */
	for (i = 0; get_pt_load(i, &phys_start, &phys_end, NULL, NULL); i++) {

		print_progress(PROGRESS_XEN_DOMAIN, i, num_pt_loads);

		pfn     = paddr_to_pfn(phys_start);
		pfn_end = paddr_to_pfn(phys_end);
		size    = pfn_end - pfn;

		for (j = 0; pfn < pfn_end; pfn++, j++) {
			print_progress(PROGRESS_XEN_DOMAIN, j + (size * i),
					size * num_pt_loads);

			if (!allocated_in_map(pfn)) {
				clear_bit_on_2nd_bitmap(pfn);
				continue;
			}

			page_info_addr = info->frame_table_vaddr + pfn * SIZE(page_info);
			if (!readmem(VADDR_XEN,
			      page_info_addr + OFFSET(page_info.count_info),
		 	      &count_info, sizeof(count_info))) {
				clear_bit_on_2nd_bitmap(pfn);
				continue;	/* page_info may not exist */
			}
			if (!readmem(VADDR_XEN,
			      page_info_addr + OFFSET(page_info._domain),
			      &_domain, sizeof(_domain))) {
				ERRMSG("Can't get page_info._domain.\n");
				return FALSE;
			}
			/*
			 * select:
			 *  - anonymous (_domain == 0), or
			 *  - xen heap area, or
			 *  - selected domain page
			 */
			if (_domain == 0)
				continue;
			if (info->xen_heap_start <= pfn && pfn < info->xen_heap_end)
				continue;
			if ((count_info & 0xffff) && is_select_domain(_domain))
				continue;
			clear_bit_on_2nd_bitmap(pfn);
		}
	}

	/*
	 * print [100 %]
	 */
	print_progress(PROGRESS_XEN_DOMAIN, num_pt_loads, num_pt_loads);
	print_execution_time(PROGRESS_XEN_DOMAIN, &tv_start);

	return TRUE;
}

int
initial_xen(void)
{
	off_t offset;
	unsigned long size;

#ifdef __powerpc__
	MSG("\n");
	MSG("ppc64 xen is not supported.\n");
	return FALSE;
#else
	if(!info->flag_elf_dumpfile) {
		MSG("Specify '-E' option for Xen.\n");
		MSG("Commandline parameter is invalid.\n");
		MSG("Try `makedumpfile --help' for more information.\n");
		return FALSE;
	}
#ifndef __x86_64__
	if (DL_EXCLUDE_ZERO < info->max_dump_level) {
		MSG("Dump_level is invalid. It should be 0 or 1.\n");
		MSG("Commandline parameter is invalid.\n");
		MSG("Try `makedumpfile --help' for more information.\n");
		return FALSE;
	}
#endif
	if (!fallback_to_current_page_size())
		return FALSE;
	/*
	 * Get the debug information for analysis from the vmcoreinfo file
	 */
	if (info->flag_read_vmcoreinfo) {
		if (!read_vmcoreinfo_xen())
			return FALSE;
		close_vmcoreinfo();
	/*
	 * Get the debug information for analysis from the xen-syms file
	 */
	} else if (info->name_xen_syms) {
		set_dwarf_debuginfo("xen-syms", NULL,
				    info->name_xen_syms, info->fd_xen_syms);

		if (!get_symbol_info_xen())
			return FALSE;
		if (!get_structure_info_xen())
			return FALSE;
	/*
	 * Get the debug information for analysis from /proc/vmcore
	 */
	} else {
		/*
		 * Check whether /proc/vmcore contains vmcoreinfo,
		 * and get both the offset and the size.
		 */
		if (!has_vmcoreinfo_xen()){
			if (!info->flag_exclude_xen_dom)
				goto out;

			MSG("%s doesn't contain a vmcoreinfo for Xen.\n",
			    info->name_memory);
			MSG("Specify '--xen-syms' option or '--xen-vmcoreinfo' option.\n");
			MSG("Commandline parameter is invalid.\n");
			MSG("Try `makedumpfile --help' for more information.\n");
			return FALSE;
		}
		/*
		 * Get the debug information from /proc/vmcore
		 */
		get_vmcoreinfo_xen(&offset, &size);
		if (!read_vmcoreinfo_from_vmcore(offset, size, TRUE))
			return FALSE;
	}
	if (!get_xen_phys_start())
		return FALSE;
	if (!get_xen_info())
		return FALSE;

	if (message_level & ML_PRINT_DEBUG_MSG)
		show_data_xen();
out:
	if (!get_max_mapnr())
		return FALSE;

	return TRUE;
#endif
}

void
print_vtop(void)
{
	unsigned long long paddr;

	if (!info->vaddr_for_vtop)
		return;

	MSG("\n");
	MSG("Translating virtual address %lx to physical address.\n", info->vaddr_for_vtop);

	paddr = vaddr_to_paddr(info->vaddr_for_vtop);

	MSG("VIRTUAL           PHYSICAL\n");
	MSG("%16lx  %llx\n", info->vaddr_for_vtop, paddr);
	MSG("\n");

	info->vaddr_for_vtop = 0;

	return;
}

void
print_report(void)
{
	unsigned long long pfn_original, pfn_excluded, shrinking;

	/*
	 * /proc/vmcore doesn't contain the memory hole area.
	 */
	pfn_original = info->max_mapnr - pfn_memhole;

	pfn_excluded = pfn_zero + pfn_cache + pfn_cache_private
	    + pfn_user + pfn_free;
	shrinking = (pfn_original - pfn_excluded) * 100;
	shrinking = shrinking / pfn_original;

	REPORT_MSG("\n");
	REPORT_MSG("Original pages  : 0x%016llx\n", pfn_original);
	REPORT_MSG("  Excluded pages   : 0x%016llx\n", pfn_excluded);
	REPORT_MSG("    Pages filled with zero  : 0x%016llx\n", pfn_zero);
	REPORT_MSG("    Cache pages             : 0x%016llx\n", pfn_cache);
	REPORT_MSG("    Cache pages + private   : 0x%016llx\n",
	    pfn_cache_private);
	REPORT_MSG("    User process data pages : 0x%016llx\n", pfn_user);
	REPORT_MSG("    Free pages              : 0x%016llx\n", pfn_free);
	REPORT_MSG("  Remaining pages  : 0x%016llx\n",
	    pfn_original - pfn_excluded);
	REPORT_MSG("  (The number of pages is reduced to %lld%%.)\n",
	    shrinking);
	REPORT_MSG("Memory Hole     : 0x%016llx\n", pfn_memhole);
	REPORT_MSG("--------------------------------------------------\n");
	REPORT_MSG("Total pages     : 0x%016llx\n", info->max_mapnr);
	REPORT_MSG("\n");
}

int
writeout_dumpfile(void)
{
	int ret = FALSE;
	struct cache_data cd_header, cd_page;

	info->flag_nospace = FALSE;

	if (!open_dump_file())
		return FALSE;

	if (info->flag_flatten) {
		if (!write_start_flat_header())
			return FALSE;
	}
	if (!prepare_cache_data(&cd_header))
		return FALSE;

	if (!prepare_cache_data(&cd_page)) {
		free_cache_data(&cd_header);
		return FALSE;
	}
	if (info->flag_elf_dumpfile) {
		if (!write_elf_header(&cd_header))
			goto out;
		if (!write_elf_pages(&cd_header, &cd_page))
			goto out;
		if (!write_elf_eraseinfo(&cd_header))
			goto out;
	} else {
		if (!write_kdump_header())
			goto out;
		if (!write_kdump_pages(&cd_header, &cd_page))
			goto out;
		if (!write_kdump_eraseinfo(&cd_page))
			goto out;
		if (!write_kdump_bitmap())
			goto out;
	}
	if (info->flag_flatten) {
		if (!write_end_flat_header())
			goto out;
	}

	ret = TRUE;
out:
	free_cache_data(&cd_header);
	free_cache_data(&cd_page);

	close_dump_file();

	if ((ret == FALSE) && info->flag_nospace)
		return NOSPACE;
	else
		return ret;
}

int
setup_splitting(void)
{
	int i;
	unsigned long long j, pfn_per_dumpfile;
	unsigned long long start_pfn, end_pfn;
	unsigned long long num_dumpable = get_num_dumpable();
	struct dump_bitmap bitmap2;

	if (info->num_dumpfile <= 1)
		return FALSE;

	initialize_2nd_bitmap(&bitmap2);

	pfn_per_dumpfile = num_dumpable / info->num_dumpfile;
	start_pfn = end_pfn = 0;
	for (i = 0; i < info->num_dumpfile; i++) {
		start_pfn = end_pfn;
		if (i == (info->num_dumpfile - 1)) {
			end_pfn  = info->max_mapnr;
		} else {
			for (j = 0; j < pfn_per_dumpfile; end_pfn++) {
				if (is_dumpable(&bitmap2, end_pfn))
					j++;
			}
		}
		SPLITTING_START_PFN(i) = start_pfn;
		SPLITTING_END_PFN(i)   = end_pfn;
	}

	return TRUE;
}

/*
 * This function is for creating split dumpfiles by multiple
 * processes. Each child process should re-open a /proc/vmcore
 * file, because it prevents each other from affectting the file
 * offset due to read(2) call.
 */
int
reopen_dump_memory()
{
	close_dump_memory();

	if ((info->fd_memory = open(info->name_memory, O_RDONLY)) < 0) {
		ERRMSG("Can't open the dump memory(%s). %s\n",
		    info->name_memory, strerror(errno));
		return FALSE;
	}
	return TRUE;
}

int
get_next_dump_level(int index)
{
	if (info->num_dump_level <= index)
		return -1;

	return info->array_dump_level[index];
}

int
delete_dumpfile(void)
{
	int i;

	if (info->flag_flatten)
		return TRUE;

	if (info->flag_split) {
		for (i = 0; i < info->num_dumpfile; i++)
			unlink(SPLITTING_DUMPFILE(i));
	} else {
		unlink(info->name_dumpfile);
	}
	return TRUE;
}

int
writeout_multiple_dumpfiles(void)
{
	int i, status, ret = TRUE;
	pid_t pid;
	pid_t array_pid[info->num_dumpfile];

	if (!setup_splitting())
		return FALSE;

	for (i = 0; i < info->num_dumpfile; i++) {
		if ((pid = fork()) < 0) {
			return FALSE;

		} else if (pid == 0) { /* Child */
			info->name_dumpfile   = SPLITTING_DUMPFILE(i);
			info->fd_bitmap       = SPLITTING_FD_BITMAP(i);
			info->split_start_pfn = SPLITTING_START_PFN(i);
			info->split_end_pfn   = SPLITTING_END_PFN(i);

			if (!reopen_dump_memory())
				exit(1);
			if ((status = writeout_dumpfile()) == FALSE)
				exit(1);
			else if (status == NOSPACE)
				exit(2);
			exit(0);
		}
		array_pid[i] = pid;
	}
	for (i = 0; i < info->num_dumpfile; i++) {
		waitpid(array_pid[i], &status, WUNTRACED);
		if (!WIFEXITED(status) || WEXITSTATUS(status) == 1) {
			ERRMSG("Child process(%d) finished imcompletely.(%d)\n",
			    array_pid[i], status);
			ret = FALSE;
		} else if ((ret == TRUE) && (WEXITSTATUS(status) == 2))
			ret = NOSPACE;
	}
	return ret;
}

static unsigned int
get_num_modules(unsigned long head, unsigned int *num)
{
	unsigned long cur;
	unsigned int num_modules = 0;

	if (!num)
		return FALSE;

	if (!readmem(VADDR, head + OFFSET(list_head.next), &cur, sizeof cur)) {
		ERRMSG("Can't get next list_head.\n");
		return FALSE;
	}
	while (cur != head) {
		num_modules++;
		if (!readmem(VADDR, cur + OFFSET(list_head.next),
					&cur, sizeof cur)) {
			ERRMSG("Can't get next list_head.\n");
			return FALSE;
		}
	}
	*num = num_modules;
	return TRUE;
}

static void
free_symbol_info(struct module_info *module)
{
	int i;

	if (module->num_syms) {
		for (i = 1; i < module->num_syms; i++)
			if (module->sym_info[i].name)
				free(module->sym_info[i].name);
		free(module->sym_info);
	}
}

static void
clean_module_symbols(void)
{
	int i;

	for (i = 0; i < mod_st.num_modules; i++)
		free_symbol_info(&mod_st.modules[i]);

	if (mod_st.num_modules) {
		free(mod_st.modules);
		mod_st.modules = NULL;
		mod_st.num_modules = 0;
	}
}

static int
__load_module_symbol(struct module_info *modules, unsigned long addr_module)
{
	int ret = FALSE;
	unsigned int nsym;
	unsigned long symtab, strtab;
	unsigned long mod_base, mod_init;
	unsigned int mod_size, mod_init_size;
	unsigned char *module_struct_mem = NULL;
	unsigned char *module_core_mem = NULL;
	unsigned char *module_init_mem = NULL;
	unsigned char *symtab_mem;
	char *module_name, *strtab_mem, *nameptr;
	unsigned int num_symtab;

	/* Allocate buffer to read struct module data from vmcore. */
	if ((module_struct_mem = calloc(1, SIZE(module))) == NULL) {
		ERRMSG("Failed to allocate buffer for module\n");
		return FALSE;
	}
	if (!readmem(VADDR, addr_module, module_struct_mem,
						SIZE(module))) {
		ERRMSG("Can't get module info.\n");
		goto out;
	}

	module_name = (char *)(module_struct_mem + OFFSET(module.name));
	if (strlen(module_name) < MOD_NAME_LEN)
		strcpy(modules->name, module_name);
	else
		strncpy(modules->name, module_name, MOD_NAME_LEN-1);

	mod_init = ULONG(module_struct_mem +
					OFFSET(module.module_init));
	mod_init_size = UINT(module_struct_mem +
					OFFSET(module.init_size));
	mod_base = ULONG(module_struct_mem +
					OFFSET(module.module_core));
	mod_size = UINT(module_struct_mem +
					OFFSET(module.core_size));

	DEBUG_MSG("Module: %s, Base: 0x%lx, Size: %u\n",
			module_name, mod_base, mod_size);
	if (mod_init_size > 0) {
		module_init_mem = calloc(1, mod_init_size);
		if (module_init_mem == NULL) {
			ERRMSG("Can't allocate memory for module "
							"init\n");
			goto out;
		}
		if (!readmem(VADDR, mod_init, module_init_mem,
						mod_init_size)) {
			ERRMSG("Can't access module init in memory.\n");
			goto out;
		}
	}

	if ((module_core_mem = calloc(1, mod_size)) == NULL) {
		ERRMSG("Can't allocate memory for module\n");
		goto out;
	}
	if (!readmem(VADDR, mod_base, module_core_mem, mod_size)) {
		ERRMSG("Can't access module in memory.\n");
		goto out;
	}

	num_symtab = UINT(module_struct_mem +
					OFFSET(module.num_symtab));
	if (!num_symtab) {
		ERRMSG("%s: Symbol info not available\n", module_name);
		goto out;
	}
	modules->num_syms = num_symtab;
	DEBUG_MSG("num_sym: %d\n", num_symtab);

	symtab = ULONG(module_struct_mem + OFFSET(module.symtab));
	strtab = ULONG(module_struct_mem + OFFSET(module.strtab));

	/* check if symtab and strtab are inside the module space. */
	if (!IN_RANGE(symtab, mod_base, mod_size) &&
		!IN_RANGE(symtab, mod_init, mod_init_size)) {
		ERRMSG("%s: module symtab is outseide of module "
			"address space\n", module_name);
		goto out;
	}
	if (IN_RANGE(symtab, mod_base, mod_size))
		symtab_mem = module_core_mem + (symtab - mod_base);
	else
		symtab_mem = module_init_mem + (symtab - mod_init);

	if (!IN_RANGE(strtab, mod_base, mod_size) &&
		!IN_RANGE(strtab, mod_init, mod_init_size)) {
		ERRMSG("%s: module strtab is outseide of module "
			"address space\n", module_name);
		goto out;
	}
	if (IN_RANGE(strtab, mod_base, mod_size))
		strtab_mem = (char *)(module_core_mem
					+ (strtab - mod_base));
	else
		strtab_mem = (char *)(module_init_mem
					+ (strtab - mod_init));

	modules->sym_info = calloc(num_symtab, sizeof(struct symbol_info));
	if (modules->sym_info == NULL) {
		ERRMSG("Can't allocate memory to store sym info\n");
		goto out;
	}

	/* symbols starts from 1 */
	for (nsym = 1; nsym < num_symtab; nsym++) {
		Elf32_Sym *sym32;
		Elf64_Sym *sym64;
		/* If case of ELF vmcore then the word size can be
		 * determined using flag_elf64_memory flag.
		 * But in case of kdump-compressed dump, kdump header
		 * does not carry word size info. May be in future
		 * this info will be available in kdump header.
		 * Until then, in order to make this logic work on both
		 * situation we depend on pointer_size that is
		 * extracted from vmlinux dwarf information.
		 */
		if ((get_pointer_size() * 8) == 64) {
			sym64 = (Elf64_Sym *) (symtab_mem
					+ (nsym * sizeof(Elf64_Sym)));
			modules->sym_info[nsym].value =
				(unsigned long long) sym64->st_value;
			nameptr = strtab_mem + sym64->st_name;
		} else {
			sym32 = (Elf32_Sym *) (symtab_mem
					+ (nsym * sizeof(Elf32_Sym)));
			modules->sym_info[nsym].value =
				(unsigned long long) sym32->st_value;
			nameptr = strtab_mem + sym32->st_name;
		}
		if (strlen(nameptr))
			modules->sym_info[nsym].name = strdup(nameptr);
		DEBUG_MSG("\t[%d] %llx %s\n", nsym,
					modules->sym_info[nsym].value, nameptr);
	}
	ret = TRUE;
out:
	free(module_struct_mem);
	free(module_core_mem);
	free(module_init_mem);

	return ret;
}

static int
load_module_symbols(void)
{
	unsigned long head, cur, cur_module;
	struct module_info *modules = NULL;
	unsigned int i = 0;

	head = SYMBOL(modules);
	if (!get_num_modules(head, &mod_st.num_modules) ||
	    !mod_st.num_modules) {
		ERRMSG("Can't get module count\n");
		return FALSE;
	}
	mod_st.modules = calloc(mod_st.num_modules,
					sizeof(struct module_info));
	if (!mod_st.modules) {
		ERRMSG("Can't allocate memory for module info\n");
		return FALSE;
	}
	modules = mod_st.modules;

	if (!readmem(VADDR, head + OFFSET(list_head.next), &cur, sizeof cur)) {
		ERRMSG("Can't get next list_head.\n");
		return FALSE;
	}

	/* Travese the list and read module symbols */
	while (cur != head) {
		cur_module = cur - OFFSET(module.list);

		if (!__load_module_symbol(&modules[i], cur_module))
			return FALSE;

		if (!readmem(VADDR, cur + OFFSET(list_head.next),
					&cur, sizeof cur)) {
			ERRMSG("Can't get next list_head.\n");
			return FALSE;
		}
		i++;
	} while (cur != head);
	return TRUE;
}

void
free_config_entry(struct config_entry *ce)
{
	struct config_entry *p;

	while(ce) {
		p = ce;
		ce = p->next;
		if (p->name)
			free(p->name);
		if (p->type_name)
			free(p->type_name);
		if (p->symbol_expr)
			free(p->symbol_expr);
		free(p);
	}
}

void
free_config(struct config *config)
{
	int i;
	if (config) {
		if (config->module_name)
			free(config->module_name);
		for (i = 0; i < config->num_filter_symbols; i++) {
			if (config->filter_symbol[i])
				free_config_entry(config->filter_symbol[i]);
			if (config->size_symbol[i])
				free_config_entry(config->size_symbol[i]);
		}
		if (config->filter_symbol)
			free(config->filter_symbol);
		if (config->size_symbol)
			free(config->size_symbol);
		free(config);
	}
}

void
print_config_entry(struct config_entry *ce)
{
	while (ce) {
		DEBUG_MSG("Name: %s\n", ce->name);
		DEBUG_MSG("Type Name: %s, ", ce->type_name);
		DEBUG_MSG("flag: %x, ", ce->flag);
		DEBUG_MSG("Type flag: %lx, ", ce->type_flag);
		DEBUG_MSG("sym_addr: %llx, ", ce->sym_addr);
		DEBUG_MSG("addr: %lx, ", ce->addr);
		DEBUG_MSG("offset: %llx, ", (unsigned long long)ce->offset);
		DEBUG_MSG("size: %ld\n", ce->size);

		ce = ce->next;
	}
}

/*
 * Read the non-terminal's which are in the form of <Symbol>[.member[...]]
 */
struct config_entry *
create_config_entry(const char *token, unsigned short flag, int line)
{
	struct config_entry *ce = NULL, *ptr, *prev_ce;
	char *str, *cur, *next;
	long len;
	int depth = 0;

	if (!token)
		return NULL;

	cur = str = strdup(token);
	prev_ce = ptr = NULL;
	while (cur != NULL) {
		if ((next = strchr(cur, '.')) != NULL) {
			*next++ = '\0';
		}
		if (!strlen(cur)) {
			cur = next;
			continue;
		}

		if ((ptr = calloc(1, sizeof(struct config_entry))) == NULL) {
			ERRMSG("Can't allocate memory for config_entry\n");
			goto err_out;
		}
		ptr->line = line;
		ptr->flag |= flag;
		if (depth == 0) {
			/* First node is always a symbol name */
			ptr->flag |= SYMBOL_ENTRY;
		}
		if (flag & ITERATION_ENTRY) {
			/* Max depth for iteration entry is 1 */
			if (depth > 0) {
				ERRMSG("Config error at %d: Invalid iteration "
					"variable entry.\n", line);
				goto err_out;
			}
			ptr->name = strdup(cur);
		}
		if (flag & (FILTER_ENTRY | LIST_ENTRY)) {
			ptr->name = strdup(cur);
		}
		if (flag & SIZE_ENTRY) {
			char ch = '\0';
			int n = 0;
			/* See if absolute length is provided */
			if ((depth == 0) &&
				((n = sscanf(cur, "%ld%c", &len, &ch)) > 0)) {
				if (len < 0) {
					ERRMSG("Config error at %d: size "
						"value must be positive.\n",
						line);
					goto err_out;
				}
				ptr->size = len;
				ptr->flag |= ENTRY_RESOLVED;
				if (n == 2) {
					/* Handle suffix.
					 * K = Kilobytes
					 * M = Megabytes
					 */
					switch (ch) {
					case 'M':
					case 'm':
						ptr->size *= 1024;
					case 'K':
					case 'k':
						ptr->size *= 1024;
						break;
					}
				}
			}
			else
				ptr->name = strdup(cur);
		}
		if (prev_ce) {
			prev_ce->next = ptr;
			prev_ce = ptr;
		}
		else
			ce = prev_ce = ptr;
		cur = next;
		depth++;
		ptr = NULL;
	}
	free(str);
	return ce;

err_out:
	if (ce)
		free_config_entry(ce);
	if (ptr)
		free_config_entry(ptr);
	free(str);
	return NULL;
}

int
is_module_loaded(char *mod_name)
{
	if (!strcmp(mod_name, "vmlinux") || get_loaded_module(mod_name))
		return TRUE;
	return FALSE;
}

/*
 * read filter config file and return each string token. If the parameter
 * expected_token is non-NULL, then return the current token if it matches
 * with expected_token otherwise save the current token and return NULL.
 * At start of every module section filter_config.new_section is set to 1 and
 * subsequent function invocations return NULL untill filter_config.new_section
 * is reset to 0 by passing @flag = CONFIG_NEW_CMD (0x02).
 *
 * Parameters:
 * @expected_token	INPUT
 *	Token string to match with currnet token.
 *	=NULL - return the current available token.
 *
 * @flag		INPUT
 *	=0x01 - Skip to next module section.
 *	=0x02 - Treat the next token as next filter command and reset.
 *
 * @line		OUTPUT
 *	Line number of current token in filter config file.
 *
 * @cur_mod		OUTPUT
 *	Points to current module section name on non-NULL return value.
 *
 * @eof			OUTPUT
 *	set to -1 when end of file is reached.
 *	set to -2 when end of section is reached.
 */
static char *
get_config_token(char *expected_token, unsigned char flag, int *line,
			char **cur_mod, int *eof)
{
	char *p;
	struct filter_config *fc = &filter_config;
	int skip = flag & CONFIG_SKIP_SECTION;

	if (!fc->file_filterconfig)
		return NULL;

	if (eof)
		*eof = 0;

	/*
	 * set token and saved_token to NULL if skip module section is set
	 * to 1.
	 */
	if (skip) {
		fc->token = NULL;
		fc->saved_token = NULL;
	}

	if (fc->saved_token) {
		fc->token = fc->saved_token;
		fc->saved_token = NULL;
	}
	else if (fc->token)
		fc->token = strtok(NULL, " ");

	/* Read next line if we are done all tokens from previous line */
	while (!fc->token && fgets(config_buf, sizeof(config_buf),
					fc->file_filterconfig)) {
		if ((p = strchr(config_buf, '\n'))) {
			*p = '\0';
			fc->line_count++;
		}
		if ((p = strchr(config_buf, '#'))) {
			*p = '\0';
		}
		/* replace all tabs with spaces */
		for (p = config_buf; *p != '\0'; p++)
			if (*p == '\t')
				*p = ' ';
		if (config_buf[0] == '[') {
			/* module section entry */
			p = strchr(config_buf, ']');
			if (!p) {
				ERRMSG("Config error at %d: Invalid module "
					"section entry.\n", fc->line_count);
				/* skip to next valid module section */
				skip = 1;
			}
			else {
				/*
				 * Found the valid module section. Reset the
				 * skip flag.
				 */
				*p = '\0';
				if (fc->cur_module)
					free(fc->cur_module);
				fc->cur_module = strdup(&config_buf[1]);
				skip = 0;
				fc->new_section = 1;
			}
			continue;
		}
		/*
		 * If symbol info for current module is not loaded then
		 * skip to next module section.
		 */
		if (skip ||
			(fc->cur_module && !is_module_loaded(fc->cur_module)))
			continue;

		fc->token = strtok(config_buf, " ");
	}
	if (!fc->token) {
		if (eof)
			*eof = -1;
		return NULL;
	}
	if (fc->new_section && !(flag & CONFIG_NEW_CMD)) {
		fc->saved_token = fc->token;
		if (eof)
			*eof = -2;
		return NULL;
	}
	else
		fc->new_section = 0;

	if (cur_mod)
		*cur_mod = fc->cur_module;

	if (line)
		*line = fc->line_count;

	if (expected_token && strcmp(fc->token, expected_token)) {
		fc->saved_token = fc->token;
		return NULL;
	}
	return fc->token;
}

static int
read_size_entry(struct config *config, int line)
{
	int idx = config->num_filter_symbols - 1;
	char *token = get_config_token(NULL, 0, &line, NULL, NULL);

	if (!token || IS_KEYWORD(token)) {
		ERRMSG("Config error at %d: expected size symbol after"
		" 'size' keyword.\n", line);
		return FALSE;
	}
	config->size_symbol[idx] = create_config_entry(token, SIZE_ENTRY, line);
	if (!config->size_symbol[idx]) {
		ERRMSG("Error at line %d: Failed to read size symbol\n",
									line);
		return FALSE;
	}
	if (config->iter_entry && config->size_symbol[idx]->name &&
					(!strcmp(config->size_symbol[idx]->name,
					config->iter_entry->name))) {
		config->size_symbol[idx]->flag &= ~SYMBOL_ENTRY;
		config->size_symbol[idx]->flag |= VAR_ENTRY;
		config->size_symbol[idx]->refer_to = config->iter_entry;
	}
	return TRUE;
}

/*
 * Read erase command entry. The erase command syntax is:
 *
 *	erase <Symbol>[.member[...]] [size <SizeValue>[K|M]]
 *	erase <Symbol>[.member[...]] [size <SizeSymbol>]
 *	erase <Symbol>[.member[...]] [nullify]
 */
static int
read_filter_entry(struct config *config, int line)
{
	int size, idx;
	char *token = get_config_token(NULL, 0, &line, NULL, NULL);

	if (!token || IS_KEYWORD(token)) {
		ERRMSG("Config error at %d: expected kernel symbol after"
		" 'erase' command.\n", line);
		return FALSE;
	}

	idx = config->num_filter_symbols;
	config->num_filter_symbols++;
	size = config->num_filter_symbols * sizeof(struct config_entry *);
	config->filter_symbol = realloc(config->filter_symbol, size);
	config->size_symbol = realloc(config->size_symbol, size);

	if (!config->filter_symbol || !config->size_symbol) {
		ERRMSG("Can't get memory to read config symbols.\n");
		return FALSE;
	}
	config->filter_symbol[idx] = NULL;
	config->size_symbol[idx] = NULL;

	config->filter_symbol[idx] =
			create_config_entry(token, FILTER_ENTRY, line);
	if (!config->filter_symbol[idx]) {
		ERRMSG("Error at line %d: Failed to read filter symbol\n",
									line);
		return FALSE;
	}

	/*
	 * Save the symbol expression string for generation of eraseinfo data
	 * later while writing dumpfile.
	 */
	config->filter_symbol[idx]->symbol_expr = strdup(token);

	if (config->iter_entry) {
		if (strcmp(config->filter_symbol[idx]->name,
				config->iter_entry->name)) {
			ERRMSG("Config error at %d: unused iteration"
				" variable '%s'.\n", line,
				config->iter_entry->name);
			return FALSE;
		}
		config->filter_symbol[idx]->flag &= ~SYMBOL_ENTRY;
		config->filter_symbol[idx]->flag |= VAR_ENTRY;
		config->filter_symbol[idx]->refer_to = config->iter_entry;
	}
	if (get_config_token("nullify", 0, &line, NULL, NULL)) {
		config->filter_symbol[idx]->nullify = 1;
	}
	else if (get_config_token("size", 0, &line, NULL, NULL)) {
		if (!read_size_entry(config, line))
			return FALSE;
	}
	return TRUE;
}

static int
add_traversal_entry(struct config_entry *ce, char *member, int line)
{
	if (!ce)
		return FALSE;

	while (ce->next)
		ce = ce->next;

	ce->next = create_config_entry(member, LIST_ENTRY, line);
	if (ce->next == NULL) {
		ERRMSG("Error at line %d: Failed to read 'via' member\n",
									line);
		return FALSE;
	}

	ce->next->flag |= TRAVERSAL_ENTRY;
	ce->next->flag &= ~SYMBOL_ENTRY;
	return TRUE;
}

static int
read_list_entry(struct config *config, int line)
{
	char *token = get_config_token(NULL, 0, &line, NULL, NULL);

	if (!token || IS_KEYWORD(token)) {
		ERRMSG("Config error at %d: expected list symbol after"
		" 'in' keyword.\n", line);
		return FALSE;
	}
	config->list_entry = create_config_entry(token, LIST_ENTRY, line);
	if (!config->list_entry) {
		ERRMSG("Error at line %d: Failed to read list symbol\n",
									line);
		return FALSE;
	}
	/* Check if user has provided 'via' or 'within' keyword */
	if (get_config_token("via", 0, &line, NULL, NULL)) {
		/* next token is traversal member NextMember */
		token = get_config_token(NULL, 0, &line, NULL, NULL);
		if (!token) {
			ERRMSG("Config error at %d: expected member name after"
			" 'via' keyword.\n", line);
			return FALSE;
		}
		if (!add_traversal_entry(config->list_entry, token, line))
			return FALSE;
	}
	else if (get_config_token("within", 0, &line, NULL, NULL)) {
		char *s_name, *lh_member;
		/* next value is StructName:ListHeadMember */
		s_name = get_config_token(NULL, 0, &line, NULL, NULL);
		if (!s_name || IS_KEYWORD(s_name)) {
			ERRMSG("Config error at %d: expected struct name after"
			" 'within' keyword.\n", line);
			return FALSE;
		}
		lh_member = strchr(s_name, ':');
		if (lh_member) {
			*lh_member++ = '\0';
			if (!strlen(lh_member)) {
				ERRMSG("Config error at %d: expected list_head"
					" member after ':'.\n", line);
				return FALSE;
			}
			config->iter_entry->next =
				create_config_entry(lh_member,
							ITERATION_ENTRY, line);
			if (!config->iter_entry->next)
				return FALSE;
			config->iter_entry->next->flag &= ~SYMBOL_ENTRY;
		}
		if (!strlen(s_name)) {
			ERRMSG("Config error at %d: Invalid token found "
				"after 'within' keyword.\n", line);
			return FALSE;
		}
		config->iter_entry->type_name = strdup(s_name);
	}
	return TRUE;
}

/*
 * Read the iteration entry (LoopConstruct). The syntax is:
 *
 *	for <id> in {<ArrayVar> |
 *		    <StructVar> via <NextMember> |
 *		    <ListHeadVar> within <StructName>:<ListHeadMember>}
 *		erase <id>[.MemberExpression] [size <SizeExpression>|nullify]
 *		[erase <id>...]
 *		[...]
 *	endfor
 */
static int
read_iteration_entry(struct config *config, int line)
{
	int eof = 0;
	char *token = get_config_token(NULL, 0, &line, NULL, NULL);

	if (!token || IS_KEYWORD(token)) {
		ERRMSG("Config error at %d: expected iteration VAR entry after"
		" 'for' keyword.\n", line);
		return FALSE;
	}
	config->iter_entry =
		create_config_entry(token, ITERATION_ENTRY, line);
	if (!config->iter_entry) {
		ERRMSG("Error at line %d: "
			"Failed to read iteration VAR entry.\n", line);
		return FALSE;
	}
	if (!get_config_token("in", 0, &line, NULL, NULL)) {
		char *token;
		token = get_config_token(NULL, 0, &line, NULL, NULL);
		if (token)
			ERRMSG("Config error at %d: Invalid token '%s'.\n",
								line, token);
		ERRMSG("Config error at %d: expected token 'in'.\n", line);
		return FALSE;
	}
	if (!read_list_entry(config, line))
		return FALSE;

	while (!get_config_token("endfor", 0, &line, NULL, &eof) && !eof) {
		if (get_config_token("erase", 0, &line, NULL, NULL)) {
			if (!read_filter_entry(config, line))
				return FALSE;
		}
		else {
			token = get_config_token(NULL, 0, &line, NULL, NULL);
			ERRMSG("Config error at %d: "
				"Invalid token '%s'.\n", line, token);
			return FALSE;
		}
	}
	if (eof) {
		ERRMSG("Config error at %d: No matching 'endfor' found.\n",
									line);
		return FALSE;
	}
	return TRUE;
}

/*
 * Configuration file 'makedumpfile.conf' contains filter commands.
 * Every individual filter command is considered as a config entry. A config
 * entry can be provided on a single line or multiple lines.
 */
struct config *
get_config(int skip)
{
	struct config *config;
	char *token = NULL;
	static int line_count = 0;
	char *cur_module = NULL;
	int eof = 0;
	unsigned char flag = CONFIG_NEW_CMD;

	if (skip)
		flag |= CONFIG_SKIP_SECTION;

	if ((config = calloc(1, sizeof(struct config))) == NULL)
		return NULL;

	if (get_config_token("erase", flag, &line_count, &cur_module, &eof)) {
		if (cur_module)
			config->module_name = strdup(cur_module);

		if (!read_filter_entry(config, line_count))
			goto err_out;
	}
	else if (get_config_token("for", 0, &line_count, &cur_module, &eof)) {
		if (cur_module)
			config->module_name = strdup(cur_module);

		if (!read_iteration_entry(config, line_count))
			goto err_out;
	}
	else {
		if (!eof) {
			token = get_config_token(NULL, 0, &line_count,
								NULL, NULL);
			ERRMSG("Config error at %d: Invalid token '%s'.\n",
							line_count, token);
		}
		goto err_out;
	}
	return config;
err_out:
	if (config)
		free_config(config);
	return NULL;
}

static unsigned long
read_pointer_value(unsigned long long addr)
{
	unsigned long val;

	if (!readmem(VADDR, addr, &val, sizeof(val))) {
		ERRMSG("Can't read pointer value\n");
		return 0;
	}
	return val;
}

int
resolve_config_entry(struct config_entry *ce, unsigned long long base_addr,
						char *base_struct_name)
{
	char buf[BUFSIZE + 1];
	unsigned long long symbol;

	if (ce->flag & SYMBOL_ENTRY) {
		/* find the symbol info */
		if (!ce->name)
			return FALSE;

		/*
		 * If we are looking for module symbol then traverse through
		 * mod_st.modules for symbol lookup
		 */
		if (sym_in_module(ce->name, &symbol))
			ce->sym_addr = symbol;
		else
			ce->sym_addr = get_symbol_addr(ce->name);
		if (!ce->sym_addr) {
			ERRMSG("Config error at %d: Can't find symbol '%s'.\n",
							ce->line, ce->name);
			return FALSE;
		}
		ce->type_name = get_symbol_type_name(ce->name,
					DWARF_INFO_GET_SYMBOL_TYPE,
					&ce->size, &ce->type_flag);
		if (ce->type_flag & TYPE_ARRAY) {
			ce->array_length = get_array_length(ce->name, NULL,
					DWARF_INFO_GET_SYMBOL_ARRAY_LENGTH);
			if (ce->array_length < 0)
				ce->array_length = 0;
		}
	}
	else if (ce->flag & VAR_ENTRY) {
		/* iteration variable.
		 * read the value from ce->refer_to
		 */
		ce->addr = ce->refer_to->addr;
		ce->sym_addr = ce->refer_to->sym_addr;
		ce->size = ce->refer_to->size;
		ce->type_flag = ce->refer_to->type_flag;
		if (!ce->type_name)
			ce->type_name = strdup(ce->refer_to->type_name);

		/* This entry has been changed hence next entry needs to
		 * be resolved accordingly.
		 */
		if (ce->next)
			ce->next->flag &= ~ENTRY_RESOLVED;
		return TRUE;
	}
	else {
		/* find the member offset */
		ce->offset = get_member_offset(base_struct_name,
				ce->name, DWARF_INFO_GET_MEMBER_OFFSET);
		ce->sym_addr = base_addr + ce->offset;
		ce->type_name = get_member_type_name(base_struct_name,
				ce->name, DWARF_INFO_GET_MEMBER_TYPE,
				&ce->size, &ce->type_flag);
		if (ce->type_flag & TYPE_ARRAY) {
			ce->array_length = get_array_length(base_struct_name,
					ce->name,
					DWARF_INFO_GET_MEMBER_ARRAY_LENGTH);
			if (ce->array_length < 0)
				ce->array_length = 0;
		}
	}
	if (ce->type_name == NULL) {
		if (!(ce->flag & SYMBOL_ENTRY))
			ERRMSG("Config error at %d: struct '%s' has no member"
				" with name '%s'.\n",
				ce->line, base_struct_name, ce->name);
		return FALSE;
	}
	if (!strcmp(ce->type_name, "list_head")) {
		ce->type_flag |= TYPE_LIST_HEAD;
		/* If this list head expression is a LIST entry then
		 * mark the next entry as TRAVERSAL_ENTRY, if any.
		 * Error out if next entry is not a last node.
		 */
		if ((ce->flag & LIST_ENTRY) && ce->next) {
			if (ce->next->next) {
				ERRMSG("Config error at %d: Only one traversal"
					" entry is allowed for list_head type"
					" LIST entry", ce->line);
				return FALSE;
			}
			ce->next->flag |= TRAVERSAL_ENTRY;
		}
	}
	ce->addr = ce->sym_addr;
	if (ce->size < 0)
		ce->size = 0;
	if ((ce->flag & LIST_ENTRY) && !ce->next) {
		/* This is the last node of LIST entry.
		 * For the list entry symbol, the allowed data types are:
		 * Array, Structure Pointer (with 'next' member) and list_head.
		 *
		 * If this is a struct or list_head data type then
		 * create a leaf node entry with 'next' member.
		 */
		if ((ce->type_flag & TYPE_BASE)
					&& (strcmp(ce->type_name, "void")))
			return FALSE;

		if ((ce->type_flag & TYPE_LIST_HEAD)
			|| ((ce->type_flag & (TYPE_STRUCT | TYPE_ARRAY))
							== TYPE_STRUCT)) {
			if (!(ce->flag & TRAVERSAL_ENTRY)) {
				ce->next = create_config_entry("next",
							LIST_ENTRY, ce->line);
				if (ce->next == NULL)
					return FALSE;

				ce->next->flag |= TRAVERSAL_ENTRY;
				ce->next->flag &= ~SYMBOL_ENTRY;
			}
		}
		if (ce->flag & TRAVERSAL_ENTRY) {
			/* type name of traversal entry should match with
			 * that of parent node.
			 */
			if (strcmp(base_struct_name, ce->type_name))
				return FALSE;
		}
	}
	if ((ce->type_flag & (TYPE_ARRAY | TYPE_PTR)) == TYPE_PTR) {
		/* If it's a pointer variable (not array) then read the
		 * pointer value. */
		ce->addr = read_pointer_value(ce->sym_addr);

		/*
		 * if it is a void pointer then reset the size to 0
		 * User need to provide a size to filter data referenced
		 * by 'void *' pointer or nullify option.
		 */
		if (!strcmp(ce->type_name, "void"))
			ce->size = 0;

	}
	if ((ce->type_flag & TYPE_BASE) && (ce->type_flag & TYPE_PTR)) {
		/*
		 * Determine the string length for 'char' pointer.
		 * BUFSIZE(1024) is the upper limit for string length.
		 */
		if (!strcmp(ce->type_name, "char")) {
			if (readmem(VADDR, ce->addr, buf, BUFSIZE)) {
				buf[BUFSIZE] = '\0';
				ce->size = strlen(buf);
			}
		}
	}
	if (!ce->next && (ce->flag & SIZE_ENTRY)) {
		void *val;

		/* leaf node of size entry */
		/* If it is size argument then update the size with data
		 * value of this symbol/member.
		 * Check if current symbol/member is of base data type.
		 */

		if (((ce->type_flag & (TYPE_ARRAY | TYPE_BASE)) != TYPE_BASE)
				|| (ce->size > sizeof(long))) {
			ERRMSG("Config error at %d: size symbol/member '%s' "
				"is not of base type.\n", ce->line, ce->name);
			return FALSE;
		}
		if ((val = calloc(1, ce->size)) == NULL) {
			ERRMSG("Can't get memory for size parameter\n");
			return FALSE;
		}

		if (!readmem(VADDR, ce->addr, val, ce->size)) {
			ERRMSG("Can't read symbol/member data value\n");
			return FALSE;
		}
		switch (ce->size) {
		case 1:
			ce->size = (long)(*((uint8_t *)val));
			break;
		case 2:
			ce->size = (long)(*((uint16_t *)val));
			break;
		case 4:
			ce->size = (long)(*((uint32_t *)val));
			break;
		case 8:
			ce->size = (long)(*((uint64_t *)val));
			break;
		}
		free(val);
	}
	ce->flag |= ENTRY_RESOLVED;
	if (ce->next)
		ce->next->flag &= ~ENTRY_RESOLVED;
	return TRUE;
}

unsigned long long
get_config_symbol_addr(struct config_entry *ce,
			unsigned long long base_addr,
			char *base_struct_name)
{
	unsigned long addr = 0;

	if (!(ce->flag & ENTRY_RESOLVED)) {
		if (!resolve_config_entry(ce, base_addr, base_struct_name))
			return 0;
	}

	if ((ce->flag & LIST_ENTRY)) {
		/* handle List entry differently */
		if (!ce->next) {
			/* leaf node. */
			if (ce->type_flag & TYPE_ARRAY) {
				if (ce->index == ce->array_length)
					return 0;
				if (!(ce->type_flag & TYPE_PTR))
					return (ce->addr +
							(ce->index * ce->size));
				/* Array of pointers.
				 *
				 * Array may contain NULL pointers at some
				 * indexes. Hence return the next non-null
				 * address value.
				 */
				while (ce->index < ce->array_length) {
					addr = read_pointer_value(ce->addr +
						(ce->index * get_pointer_size()));
					ce->index++;
					if (addr)
						break;
				}
				return addr;
			}
			else {
				if (ce->addr == ce->cmp_addr)
					return 0;

				/* Set the leaf node as unresolved, so that
				 * it will be resolved every time when
				 * get_config_symbol_addr is called untill
				 * it hits the exit condiftion.
				 */
				ce->flag &= ~ENTRY_RESOLVED;
			}
		}
		else if ((ce->next->next == NULL) &&
					!(ce->next->type_flag & TYPE_ARRAY)) {
			/* the next node is leaf node. for non-array element
			 * Set the sym_addr and addr of this node with that of
			 * leaf node.
			 */
			addr = ce->addr;
			ce->addr = ce->next->addr;

			if (!(ce->type_flag & TYPE_LIST_HEAD)) {
				if (addr == ce->next->cmp_addr)
					return 0;

				if (!ce->next->cmp_addr) {
					/* safeguard against circular
					 * link-list
					 */
					ce->next->cmp_addr = addr;
				}

				/* Force resolution of traversal node */
				if (ce->addr && !resolve_config_entry(ce->next,
						ce->addr, ce->type_name))
					return 0;

				return addr;
			}
		}
	}

	if (ce->next && ce->addr) {
		/* Populate nullify flag down the list */
		ce->next->nullify = ce->nullify;
		return get_config_symbol_addr(ce->next, ce->addr,
							ce->type_name);
	}
	else if (!ce->next && ce->nullify) {
		/* nullify is applicable to pointer type */
		if (ce->type_flag & TYPE_PTR)
			return ce->sym_addr;
		else
			return 0;
	}
	else
		return ce->addr;
}

long
get_config_symbol_size(struct config_entry *ce,
			unsigned long long base_addr,
			char *base_struct_name)
{
	if (!(ce->flag & ENTRY_RESOLVED)) {
		if (!resolve_config_entry(ce, base_addr, base_struct_name))
			return 0;
	}

	if (ce->next && ce->addr)
		return get_config_symbol_size(ce->next, ce->addr,
							ce->type_name);
	else {
		if (ce->type_flag & TYPE_ARRAY) {
			if (ce->type_flag & TYPE_PTR)
				return ce->array_length * get_pointer_size();
			else
				return ce->array_length * ce->size;
		}
		return ce->size;
	}
}

static int
resolve_list_entry(struct config_entry *ce, unsigned long long base_addr,
			char *base_struct_name, char **out_type_name,
			unsigned char *out_type_flag)
{
	if (!(ce->flag & ENTRY_RESOLVED)) {
		if (!resolve_config_entry(ce, base_addr, base_struct_name))
			return FALSE;
	}

	if (ce->next && (ce->next->flag & TRAVERSAL_ENTRY) &&
				(ce->type_flag & TYPE_ARRAY)) {
		/*
		 * We are here because user has provided
		 * traversal member for ArrayVar using 'via' keyword.
		 *
		 * Print warning and continue.
		 */
		ERRMSG("Warning: line %d: 'via' keyword not required "
			"for ArrayVar.\n", ce->next->line);
		free_config_entry(ce->next);
		ce->next = NULL;
	}
	if ((ce->type_flag & TYPE_LIST_HEAD) && ce->next &&
			(ce->next->flag & TRAVERSAL_ENTRY)) {
		/* set cmp_addr for list empty condition.  */
		ce->next->cmp_addr = ce->sym_addr;
	}
	if (ce->next && ce->addr) {
		return resolve_list_entry(ce->next, ce->addr,
				ce->type_name, out_type_name, out_type_flag);
	}
	else {
		ce->index = 0;
		if (out_type_name)
			*out_type_name = ce->type_name;
		if (out_type_flag)
			*out_type_flag = ce->type_flag;
	}
	return TRUE;
}

/*
 * Insert the filter info node using insertion sort.
 * If filter node for a given paddr is aready present then update the size
 * and delete the fl_info node passed.
 *
 * Return 1 on successfull insertion.
 * Return 0 if filter node with same paddr is found.
 */
int
insert_filter_info(struct filter_info *fl_info)
{
	struct filter_info *prev = NULL;
	struct filter_info *ptr = filter_info;

	if (!ptr) {
		filter_info = fl_info;
		return 1;
	}

	while (ptr) {
		if (fl_info->paddr <= ptr->paddr)
			break;
		prev = ptr;
		ptr = ptr->next;
	}
	if (ptr && (fl_info->paddr == ptr->paddr)) {
		if (fl_info->size > ptr->size)
			ptr->size = fl_info->size;
		free(fl_info);
		return 0;
	}

	if (prev) {
		fl_info->next = ptr;
		prev->next = fl_info;
	}
	else {
		fl_info->next = filter_info;
		filter_info = fl_info;
	}
	return 1;
}

/*
 * Create an erase info node for each erase command. One node per erase
 * command even if it is part of loop construct.
 * For erase commands that are not part of loop construct, the num_sizes will
 * always be 1
 * For erase commands that are part of loop construct, the num_sizes may be
 * 1 or >1 depending on number iterations. This function will called multiple
 * times depending on iterations. At first invokation create a node and
 * increment num_sizes for subsequent invokations.
 *
 * The valid erase info node starts from index value 1. (index 0 is invalid
 * index).
 *
 *            Index 0     1        2        3
 *             +------+--------+--------+--------+
 * erase_info->|Unused|        |        |        |......
 *             +------+--------+--------+--------+
 *                        |        .        .        .....
 *                        V
 *                   +---------+
 *                   | char*   |----> Original erase command string
 *                   +---------+
 *                   |num_sizes|
 *                   +---------+      +--+--+--+
 *                   | sizes   |----> |  |  |  |... Sizes array of num_sizes
 *                   +---------+      +--+--+--+
 *
 * On success, return the index value of erase node for given erase command.
 * On failure, return 0.
 */
static int
add_erase_info_node(struct config_entry *filter_symbol)
{
	int idx = filter_symbol->erase_info_idx;

	/*
	 * Check if node is already created, if yes, increment the num_sizes.
	 */
	if (idx) {
		erase_info[idx].num_sizes++;
		return idx;
	}

	/* Allocate a new node. */
	DEBUG_MSG("Allocating new erase info node for command \"%s\"\n",
			filter_symbol->symbol_expr);
	idx = num_erase_info++;
	erase_info = realloc(erase_info,
			sizeof(struct erase_info) * num_erase_info);
	if (!erase_info) {
		ERRMSG("Can't get memory to create erase information.\n");
		return 0;
	}

	memset(&erase_info[idx], 0, sizeof(struct erase_info));
	erase_info[idx].symbol_expr = filter_symbol->symbol_expr;
	erase_info[idx].num_sizes = 1;

	filter_symbol->symbol_expr = NULL;
	filter_symbol->erase_info_idx = idx;

	return idx;
}

/* Return the index value in sizes array for given erase command index. */
static inline int
get_size_index(int ei_idx)
{
	if (ei_idx)
		return erase_info[ei_idx].num_sizes - 1;
	return 0;
}

int
update_filter_info(struct config_entry *filter_symbol,
			struct config_entry *size_symbol)
{
	unsigned long long addr;
	long size;
	struct filter_info *fl_info;

	addr = get_config_symbol_addr(filter_symbol, 0, NULL);
	if (message_level & ML_PRINT_DEBUG_MSG)
		print_config_entry(filter_symbol);
	if (!addr)
		return FALSE;

	if (filter_symbol->nullify)
		size = get_pointer_size();
	else if (size_symbol) {
		size = get_config_symbol_size(size_symbol, 0, NULL);
		if (message_level & ML_PRINT_DEBUG_MSG)
			print_config_entry(size_symbol);
	}
	else
		size = get_config_symbol_size(filter_symbol, 0, NULL);

	if (size <= 0)
		return FALSE;

	if ((fl_info = calloc(1, sizeof(struct filter_info))) == NULL) {
		ERRMSG("Can't allocate filter info\n");
		return FALSE;
	}
	fl_info->address = addr;
	fl_info->paddr = vaddr_to_paddr(addr);
	fl_info->size = size;
	fl_info->nullify = filter_symbol->nullify;

	if (insert_filter_info(fl_info)) {
		fl_info->erase_info_idx = add_erase_info_node(filter_symbol);
		fl_info->size_idx = get_size_index(fl_info->erase_info_idx);
	}
	return TRUE;
}

int
initialize_iteration_entry(struct config_entry *ie,
				char *type_name, unsigned char type_flag)
{
	if (!(ie->flag & ITERATION_ENTRY))
		return FALSE;

	if (type_flag & TYPE_LIST_HEAD) {
		if (!ie->type_name) {
			ERRMSG("Config error at %d: Use 'within' keyword "
				"to specify StructName:ListHeadMember.\n",
				ie->line);
			return FALSE;
		}
		/*
		 * If the LIST entry is of list_head type and user has not
		 * specified the member name where iteration entry is hooked
		 * on to list_head, then we default to member name 'list'.
		 */
		if (!ie->next) {
			ie->next = create_config_entry("list", ITERATION_ENTRY,
								ie->line);
			ie->next->flag &= ~SYMBOL_ENTRY;
		}
	}
	else {
		if (ie->type_name) {
			/* looks like user has used 'within' keyword for
			 * non-list_head VAR. Print the warning and continue.
			 */
			ERRMSG("Warning: line %d: 'within' keyword not "
				"required for ArrayVar/StructVar.\n", ie->line);
			free(ie->type_name);

			/* remove the next list_head member from iteration
			 * entry that would have added as part of 'within'
			 * keyword processing.
			 */
			if (ie->next) {
				free_config_entry(ie->next);
				ie->next = NULL;
			}
		}
		ie->type_name = strdup(type_name);
	}

	if (!ie->size) {
		ie->size = get_structure_size(ie->type_name,
						DWARF_INFO_GET_STRUCT_SIZE);
		if (ie->size == FAILED_DWARFINFO) {
			ERRMSG("Config error at %d: "
				"Can't get size for type: %s.\n",
				ie->line, ie->type_name);
			return FALSE;
		}
		else if (ie->size == NOT_FOUND_STRUCTURE) {
			ERRMSG("Config error at %d: "
				"Can't find structure: %s.\n",
				ie->line, ie->type_name);
			return FALSE;
		}
	}
	if (type_flag & TYPE_LIST_HEAD) {
		if (!resolve_config_entry(ie->next, 0, ie->type_name))
			return FALSE;

		if (strcmp(ie->next->type_name, "list_head")) {
			ERRMSG("Config error at %d: "
				"Member '%s' is not of 'list_head' type.\n",
				ie->next->line, ie->next->name);
			return FALSE;
		}
	}
	return TRUE;
}

int
list_entry_empty(struct config_entry *le, struct config_entry *ie)
{
	unsigned long long addr;

	/* Error out if arguments are not correct */
	if (!(le->flag & LIST_ENTRY) || !(ie->flag & ITERATION_ENTRY)) {
		ERRMSG("Invalid arguments\n");
		return TRUE;
	}
	addr = get_config_symbol_addr(le, 0, NULL);
	if (!addr)
		return TRUE;

	if (ie->next) {
		/* we are dealing with list_head */
		ie->next->addr = addr;
		ie->addr = addr - ie->next->offset;
		//resolve_iteration_entry(ie, addr);
	}
	else
		ie->addr = addr;
	return FALSE;
}

/*
 * Process the config entry that has been read by get_config.
 * return TRUE on success
 */
int
process_config(struct config *config)
{
	int i;
	if (config->list_entry) {
		unsigned char type_flag;
		char *type_name = NULL;
		/*
		 * We are dealing with 'for' command.
		 * - First resolve list entry.
		 * - Initialize iteration entry for iteration.
		 * - Populate iteration entry untill list entry empty.
		 */
		if (!resolve_list_entry(config->list_entry, 0, NULL,
					&type_name, &type_flag)) {
			return FALSE;
		}
		if (!initialize_iteration_entry(config->iter_entry,
						type_name, type_flag)) {
			return FALSE;
		}

		while (!list_entry_empty(config->list_entry,
						config->iter_entry)) {
			for (i = 0; i < config->num_filter_symbols; i++)
				update_filter_info(config->filter_symbol[i],
							config->size_symbol[i]);
		}
	}
	else
		update_filter_info(config->filter_symbol[0],
						config->size_symbol[0]);

	return TRUE;
}

void
print_filter_info()
{
	struct filter_info *fl_info = filter_info;

	DEBUG_MSG("\n");
	while (fl_info) {
		DEBUG_MSG("filter address: paddr (%llx), sym_addr (%llx),"
			" Size (%ld)\n",
			fl_info->paddr, fl_info->address, fl_info->size);
		fl_info = fl_info->next;
	}
}

void
init_filter_config()
{
	filter_config.name_filterconfig = info->name_filterconfig;
	filter_config.file_filterconfig = info->file_filterconfig;
	filter_config.saved_token = NULL;
	filter_config.token = NULL;
	filter_config.cur_module = NULL;
	filter_config.new_section = 0;
	filter_config.line_count = 0;
}

/*
 * Read and process each config entry (filter commands) from filter config
 * file. If no module debuginfo found for specified module section then skip
 * to next module section.
 */
int
process_config_file(const char *name_config)
{
	struct config *config;
	int skip_section = 0;

	if (!name_config)
		return FALSE;

	if ((info->file_filterconfig = fopen(name_config, "r")) == NULL) {
		ERRMSG("Can't open config file(%s). %s\n",
		    name_config, strerror(errno));
		return FALSE;
	}

	init_filter_config();

	while((config = get_config(skip_section)) != NULL) {
		skip_section = 0;
		if (config->module_name &&
				strcmp(config->module_name, "vmlinux")) {
			/*
			 * if Module debuginfo is not available, then skip to
			 * next module section.
			 */
			if (!set_dwarf_debuginfo(config->module_name,
				  info->system_utsname.release, NULL, -1)) {
				ERRMSG("Skipping to next Module section\n");
				skip_section = 1;
				free_config(config);
				continue;
			}
		}
		else {
			set_dwarf_debuginfo("vmlinux", NULL,
				info->name_vmlinux, info->fd_vmlinux);
		}
		process_config(config);
		free_config(config);
	}

	fclose(info->file_filterconfig);
	print_filter_info();
	return TRUE;
}

int
gather_filter_info()
{
	int ret;

	/*
	 * Before processing filter config file, load the symbol data of
	 * loaded modules from vmcore.
	 */
	set_dwarf_debuginfo("vmlinux", NULL,
			    info->name_vmlinux, info->fd_vmlinux);
	if (!load_module_symbols())
		return FALSE;

	ret = process_config_file(info->name_filterconfig);

	/*
	 * Remove modules symbol information, we dont need now.
	 * Reset the dwarf debuginfo to vmlinux to close open file
	 * descripter of module debuginfo file, if any.
	 */
	clean_module_symbols();
	set_dwarf_debuginfo("vmlinux", NULL,
			    info->name_vmlinux, info->fd_vmlinux);
	return ret;
}

void
clear_filter_info(void)
{
	struct filter_info *prev, *fi = filter_info;
	int i;

	/* Delete filter_info nodes that are left out. */
	while (fi) {
		prev = fi;
		fi = fi->next;
		free(prev);
	}
	filter_info = NULL;
	if (erase_info) {
		for (i = 1; i < num_erase_info; i++) {
			free(erase_info[i].symbol_expr);
			free(erase_info[i].sizes);
		}
		free(erase_info);
	}
}

int
create_dumpfile(void)
{
	int num_retry, status, new_level;

	if (!open_files_for_creating_dumpfile())
		return FALSE;

	if (!info->flag_refiltering) {
		if (!get_elf_info(info->fd_memory, info->name_memory))
			return FALSE;
	}
	if (is_xen_memory()) {
		if (!initial_xen())
			return FALSE;
	}
	if (!initial())
		return FALSE;

	print_vtop();

	num_retry = 0;
retry:
	if (info->flag_refiltering) {
		/* Change dump level */
		new_level = info->dump_level | info->kh_memory->dump_level;
		if (new_level != info->dump_level) {
			info->dump_level = new_level;
			MSG("dump_level is changed to %d, " \
				"because %s was created by dump_level(%d).",
				new_level, info->name_memory,
				info->kh_memory->dump_level);
		}
	}

	if (info->name_filterconfig && !gather_filter_info())
		return FALSE;

	if (!create_dump_bitmap())
		return FALSE;

	if (info->flag_split) {
		if ((status = writeout_multiple_dumpfiles()) == FALSE)
			return FALSE;
	} else {
		if ((status = writeout_dumpfile()) == FALSE)
			return FALSE;
	}
	if (status == NOSPACE) {
		/*
		 * If specifying the other dump_level, makedumpfile tries
		 * to create a dumpfile with it again.
		 */
		num_retry++;
		if ((info->dump_level = get_next_dump_level(num_retry)) < 0)
 			return FALSE;
		MSG("Retry to create a dumpfile by dump_level(%d).\n",
		    info->dump_level);
		if (!delete_dumpfile())
 			return FALSE;
		goto retry;
	}
	print_report();

	clear_filter_info();
	if (!close_files_for_creating_dumpfile())
		return FALSE;

	return TRUE;
}

int
__read_disk_dump_header(struct disk_dump_header *dh, char *filename)
{
	int fd, ret = FALSE;

	if ((fd = open(filename, O_RDONLY)) < 0) {
		ERRMSG("Can't open a file(%s). %s\n",
		    filename, strerror(errno));
		return FALSE;
	}
	if (lseek(fd, 0x0, SEEK_SET) < 0) {
		ERRMSG("Can't seek a file(%s). %s\n",
		    filename, strerror(errno));
		goto out;
	}
	if (read(fd, dh, sizeof(struct disk_dump_header))
	    != sizeof(struct disk_dump_header)) {
		ERRMSG("Can't read a file(%s). %s\n",
		    filename, strerror(errno));
		goto out;
	}
	ret = TRUE;
out:
	close(fd);

	return ret;
}

int
read_disk_dump_header(struct disk_dump_header *dh, char *filename)
{
	if (!__read_disk_dump_header(dh, filename))
		return FALSE;

	if (strncmp(dh->signature, KDUMP_SIGNATURE, strlen(KDUMP_SIGNATURE))) {
		ERRMSG("%s is not the kdump-compressed format.\n",
		    filename);
		return FALSE;
	}
	return TRUE;
}

int
read_kdump_sub_header(struct kdump_sub_header *kh, char *filename)
{
	int fd, ret = FALSE;
	struct disk_dump_header dh;
	off_t offset;

	if (!read_disk_dump_header(&dh, filename))
		return FALSE;

	offset = DISKDUMP_HEADER_BLOCKS * dh.block_size;

	if ((fd = open(filename, O_RDONLY)) < 0) {
		ERRMSG("Can't open a file(%s). %s\n",
		    filename, strerror(errno));
		return FALSE;
	}
	if (lseek(fd, offset, SEEK_SET) < 0) {
		ERRMSG("Can't seek a file(%s). %s\n",
		    filename, strerror(errno));
		goto out;
	}
	if (read(fd, kh, sizeof(struct kdump_sub_header))
	     != sizeof(struct kdump_sub_header)) {
		ERRMSG("Can't read a file(%s). %s\n",
		    filename, strerror(errno));
		goto out;
	}
	ret = TRUE;
out:
	close(fd);

	return ret;
}

int
store_splitting_info(void)
{
	int i;
	struct disk_dump_header dh, tmp_dh;
	struct kdump_sub_header kh;

	for (i = 0; i < info->num_dumpfile; i++) {
		if (!read_disk_dump_header(&tmp_dh, SPLITTING_DUMPFILE(i)))
			return FALSE;

		if (i == 0) {
			memcpy(&dh, &tmp_dh, sizeof(tmp_dh));
			info->max_mapnr = dh.max_mapnr;
			if (!set_page_size(dh.block_size))
				return FALSE;
			DEBUG_MSG("max_mapnr    : %llx\n", info->max_mapnr);
			DEBUG_MSG("page_size    : %ld\n", info->page_size);
		}

		/*
		 * Check whether multiple dumpfiles are parts of
		 * the same /proc/vmcore.
		 */
		if (memcmp(&dh, &tmp_dh, sizeof(tmp_dh))) {
			ERRMSG("Invalid dumpfile(%s).\n",
			    SPLITTING_DUMPFILE(i));
			return FALSE;
		}
		if (!read_kdump_sub_header(&kh, SPLITTING_DUMPFILE(i)))
			return FALSE;

		if (i == 0) {
			info->dump_level = kh.dump_level;
			DEBUG_MSG("dump_level   : %d\n", info->dump_level);
		}
		SPLITTING_START_PFN(i) = kh.start_pfn;
		SPLITTING_END_PFN(i)   = kh.end_pfn;
		SPLITTING_OFFSET_EI(i) = kh.offset_eraseinfo;
		SPLITTING_SIZE_EI(i)   = kh.size_eraseinfo;
	}
	return TRUE;
}

void
sort_splitting_info(void)
{
	int i, j;
	unsigned long long start_pfn, end_pfn;
	char *name_dumpfile;

	/*
	 * Sort splitting_info by start_pfn.
	 */
	for (i = 0; i < (info->num_dumpfile - 1); i++) {
		for (j = i; j < info->num_dumpfile; j++) {
			if (SPLITTING_START_PFN(i) < SPLITTING_START_PFN(j))
				continue;
			start_pfn     = SPLITTING_START_PFN(i);
			end_pfn       = SPLITTING_END_PFN(i);
			name_dumpfile = SPLITTING_DUMPFILE(i);

			SPLITTING_START_PFN(i) = SPLITTING_START_PFN(j);
			SPLITTING_END_PFN(i)   = SPLITTING_END_PFN(j);
			SPLITTING_DUMPFILE(i)  = SPLITTING_DUMPFILE(j);

			SPLITTING_START_PFN(j) = start_pfn;
			SPLITTING_END_PFN(j)   = end_pfn;
			SPLITTING_DUMPFILE(j)  = name_dumpfile;
		}
	}

	DEBUG_MSG("num_dumpfile : %d\n", info->num_dumpfile);
	for (i = 0; i < info->num_dumpfile; i++) {
		DEBUG_MSG("dumpfile (%s)\n", SPLITTING_DUMPFILE(i));
		DEBUG_MSG("  start_pfn  : %llx\n", SPLITTING_START_PFN(i));
		DEBUG_MSG("  end_pfn    : %llx\n", SPLITTING_END_PFN(i));
	}
}

int
check_splitting_info(void)
{
	int i;
	unsigned long long end_pfn;

	/*
	 * Check whether there are not lack of /proc/vmcore.
	 */
	if (SPLITTING_START_PFN(0) != 0) {
		ERRMSG("There is not dumpfile corresponding to pfn 0x%x - 0x%llx.\n",
		    0x0, SPLITTING_START_PFN(0));
		return FALSE;
	}
	end_pfn = SPLITTING_END_PFN(0);

	for (i = 1; i < info->num_dumpfile; i++) {
		if (end_pfn != SPLITTING_START_PFN(i)) {
			ERRMSG("There is not dumpfile corresponding to pfn 0x%llx - 0x%llx.\n",
			    end_pfn, SPLITTING_START_PFN(i));
			return FALSE;
		}
		end_pfn = SPLITTING_END_PFN(i);
	}
	if (end_pfn != info->max_mapnr) {
		ERRMSG("There is not dumpfile corresponding to pfn 0x%llx - 0x%llx.\n",
		    end_pfn, info->max_mapnr);
		return FALSE;
	}

	return TRUE;
}

int
get_splitting_info(void)
{
	if (!store_splitting_info())
		return FALSE;

	sort_splitting_info();

	if (!check_splitting_info())
		return FALSE;

	if (!get_kdump_compressed_header_info(SPLITTING_DUMPFILE(0)))
		return FALSE;

	return TRUE;
}

int
copy_same_data(int src_fd, int dst_fd, off_t offset, unsigned long size)
{
	int ret = FALSE;
	char *buf = NULL;

	if ((buf = malloc(size)) == NULL) {
		ERRMSG("Can't allcate memory.\n");
		return FALSE;
	}
	if (lseek(src_fd, offset, SEEK_SET) < 0) {
		ERRMSG("Can't seek a source file. %s\n", strerror(errno));
		goto out;
	}
	if (read(src_fd, buf, size) != size) {
		ERRMSG("Can't read a source file. %s\n", strerror(errno));
		goto out;
	}
	if (lseek(dst_fd, offset, SEEK_SET) < 0) {
		ERRMSG("Can't seek a destination file. %s\n", strerror(errno));
		goto out;
	}
	if (write(dst_fd, buf, size) != size) {
		ERRMSG("Can't write a destination file. %s\n", strerror(errno));
		goto out;
	}
	ret = TRUE;
out:
	free(buf);
	return ret;
}

int
reassemble_kdump_header(void)
{
	int fd = -1, ret = FALSE;
	off_t offset;
	unsigned long size;
	struct disk_dump_header dh;
	struct kdump_sub_header kh;
	char *buf_bitmap = NULL;

	/*
	 * Write common header.
	 */
	if (!read_disk_dump_header(&dh, SPLITTING_DUMPFILE(0)))
		return FALSE;

	if (lseek(info->fd_dumpfile, 0x0, SEEK_SET) < 0) {
		ERRMSG("Can't seek a file(%s). %s\n",
		    info->name_dumpfile, strerror(errno));
		return FALSE;
	}
	if (write(info->fd_dumpfile, &dh, sizeof(dh)) != sizeof(dh)) {
		ERRMSG("Can't write a file(%s). %s\n",
		    info->name_dumpfile, strerror(errno));
		return FALSE;
	}

	/*
	 * Write sub header.
	 */
	if (!read_kdump_sub_header(&kh, SPLITTING_DUMPFILE(0)))
		return FALSE;

	kh.split = 0;
	kh.start_pfn = 0;
	kh.end_pfn   = 0;

	if (lseek(info->fd_dumpfile, info->page_size, SEEK_SET) < 0) {
		ERRMSG("Can't seek a file(%s). %s\n",
		    info->name_dumpfile, strerror(errno));
		return FALSE;
	}
	if (write(info->fd_dumpfile, &kh, sizeof(kh)) != sizeof(kh)) {
		ERRMSG("Can't write a file(%s). %s\n",
		    info->name_dumpfile, strerror(errno));
		return FALSE;
	}
	memcpy(&info->sub_header, &kh, sizeof(kh));

	if ((fd = open(SPLITTING_DUMPFILE(0), O_RDONLY)) < 0) {
		ERRMSG("Can't open a file(%s). %s\n",
		    SPLITTING_DUMPFILE(0), strerror(errno));
		return FALSE;
	}
	if (has_pt_note()) {
		get_pt_note(&offset, &size);
		if (!copy_same_data(fd, info->fd_dumpfile, offset, size)) {
			ERRMSG("Can't copy pt_note data to %s.\n",
			    info->name_dumpfile);
			goto out;
		}
	}
	if (has_vmcoreinfo()) {
		get_vmcoreinfo(&offset, &size);
		if (!copy_same_data(fd, info->fd_dumpfile, offset, size)) {
			ERRMSG("Can't copy vmcoreinfo data to %s.\n",
			    info->name_dumpfile);
			goto out;
		}
	}

	/*
	 * Write dump bitmap to both a dumpfile and a bitmap file.
	 */
	offset = (DISKDUMP_HEADER_BLOCKS + dh.sub_hdr_size) * dh.block_size;
	info->len_bitmap = dh.bitmap_blocks * dh.block_size;
	if ((buf_bitmap = malloc(info->len_bitmap)) == NULL) {
		ERRMSG("Can't allcate memory for bitmap.\n");
		goto out;
	}
	if (lseek(fd, offset, SEEK_SET) < 0) {
		ERRMSG("Can't seek a file(%s). %s\n",
		    SPLITTING_DUMPFILE(0), strerror(errno));
		goto out;
	}
	if (read(fd, buf_bitmap, info->len_bitmap) != info->len_bitmap) {
		ERRMSG("Can't read a file(%s). %s\n",
		    SPLITTING_DUMPFILE(0), strerror(errno));
		goto out;
	}

	if (lseek(info->fd_dumpfile, offset, SEEK_SET) < 0) {
		ERRMSG("Can't seek a file(%s). %s\n",
		    info->name_dumpfile, strerror(errno));
		goto out;
	}
	if (write(info->fd_dumpfile, buf_bitmap, info->len_bitmap)
	    != info->len_bitmap) {
		ERRMSG("Can't write a file(%s). %s\n",
		    info->name_dumpfile, strerror(errno));
		goto out;
	}

	if (lseek(info->fd_bitmap, 0x0, SEEK_SET) < 0) {
		ERRMSG("Can't seek a file(%s). %s\n",
		    info->name_bitmap, strerror(errno));
		goto out;
	}
	if (write(info->fd_bitmap, buf_bitmap, info->len_bitmap)
	    != info->len_bitmap) {
		ERRMSG("Can't write a file(%s). %s\n",
		    info->name_bitmap, strerror(errno));
		goto out;
	}

	ret = TRUE;
out:
	if (fd > 0)
		close(fd);
	free(buf_bitmap);

	return ret;
}

int
reassemble_kdump_pages(void)
{
	int i, fd = 0, ret = FALSE;
	off_t offset_first_ph, offset_ph_org, offset_eraseinfo;
	off_t offset_data_new, offset_zero_page = 0;
	unsigned long long pfn, start_pfn, end_pfn;
	unsigned long long num_dumpable, num_dumped;
	unsigned long size_eraseinfo;
	struct dump_bitmap bitmap2;
	struct disk_dump_header dh;
	struct page_desc pd, pd_zero;
	struct cache_data cd_pd, cd_data;
	struct timeval tv_start;
	char *data = NULL;
	unsigned long data_buf_size = info->page_size;

	initialize_2nd_bitmap(&bitmap2);

	if (!read_disk_dump_header(&dh, SPLITTING_DUMPFILE(0)))
		return FALSE;

	if (!prepare_cache_data(&cd_pd))
		return FALSE;

	if (!prepare_cache_data(&cd_data)) {
		free_cache_data(&cd_pd);
		return FALSE;
	}
	if ((data = malloc(data_buf_size)) == NULL) {
		ERRMSG("Can't allcate memory for page data.\n");
		free_cache_data(&cd_pd);
		free_cache_data(&cd_data);
		return FALSE;
	}
	num_dumpable = get_num_dumpable();
	num_dumped = 0;

	offset_first_ph
	    = (DISKDUMP_HEADER_BLOCKS + dh.sub_hdr_size + dh.bitmap_blocks)
		* dh.block_size;
	cd_pd.offset    = offset_first_ph;
	offset_data_new = offset_first_ph + sizeof(page_desc_t) * num_dumpable;
	cd_data.offset  = offset_data_new;

	/*
	 * Write page header of zero-filled page.
	 */
	gettimeofday(&tv_start, NULL);
	if (info->dump_level & DL_EXCLUDE_ZERO) {
		/*
		 * makedumpfile outputs the data of zero-filled page at first
		 * if excluding zero-filled page, so the offset of first data
		 * is for zero-filled page in all dumpfiles.
		 */
		offset_zero_page = offset_data_new;

		pd_zero.size = info->page_size;
		pd_zero.flags = 0;
		pd_zero.offset = offset_data_new;
		pd_zero.page_flags = 0;
		memset(data, 0, pd_zero.size);
		if (!write_cache(&cd_data, data, pd_zero.size))
			goto out;
		offset_data_new  += pd_zero.size;
	}

	for (i = 0; i < info->num_dumpfile; i++) {
		if ((fd = open(SPLITTING_DUMPFILE(i), O_RDONLY)) < 0) {
			ERRMSG("Can't open a file(%s). %s\n",
			    SPLITTING_DUMPFILE(i), strerror(errno));
			goto out;
		}
		start_pfn = SPLITTING_START_PFN(i);
		end_pfn   = SPLITTING_END_PFN(i);

		offset_ph_org = offset_first_ph;
		for (pfn = start_pfn; pfn < end_pfn; pfn++) {
			if (!is_dumpable(&bitmap2, pfn))
				continue;

			num_dumped++;

			print_progress(PROGRESS_COPY, num_dumped, num_dumpable);

			if (lseek(fd, offset_ph_org, SEEK_SET) < 0) {
				ERRMSG("Can't seek a file(%s). %s\n",
				    SPLITTING_DUMPFILE(i), strerror(errno));
				goto out;
			}
			if (read(fd, &pd, sizeof(pd)) != sizeof(pd)) {
				ERRMSG("Can't read a file(%s). %s\n",
				    SPLITTING_DUMPFILE(i), strerror(errno));
				goto out;
			}
			if (lseek(fd, pd.offset, SEEK_SET) < 0) {
				ERRMSG("Can't seek a file(%s). %s\n",
				    SPLITTING_DUMPFILE(i), strerror(errno));
				goto out;
			}
			if (read(fd, data, pd.size) != pd.size) {
				ERRMSG("Can't read a file(%s). %s\n",
				    SPLITTING_DUMPFILE(i), strerror(errno));
				goto out;
			}
			if ((info->dump_level & DL_EXCLUDE_ZERO)
			    && (pd.offset == offset_zero_page)) {
				/*
			 	 * Handle the data of zero-filled page.
				 */
				if (!write_cache(&cd_pd, &pd_zero,
				    sizeof(pd_zero)))
					goto out;
				offset_ph_org += sizeof(pd);
				continue;
			}
			pd.offset = offset_data_new;
			if (!write_cache(&cd_pd, &pd, sizeof(pd)))
				goto out;
			offset_ph_org += sizeof(pd);

			if (!write_cache(&cd_data, data, pd.size))
				goto out;

			offset_data_new += pd.size;
		}
		close(fd);
		fd = 0;
	}
	if (!write_cache_bufsz(&cd_pd))
		goto out;
	if (!write_cache_bufsz(&cd_data))
		goto out;

	offset_eraseinfo = cd_data.offset;
	size_eraseinfo   = 0;
	/* Copy eraseinfo from split dumpfiles to o/p dumpfile */
	for (i = 0; i < info->num_dumpfile; i++) {
		if (!SPLITTING_SIZE_EI(i))
			continue;

		if (SPLITTING_SIZE_EI(i) > data_buf_size) {
			data_buf_size = SPLITTING_SIZE_EI(i);
			if ((data = realloc(data, data_buf_size)) == NULL) {
				ERRMSG("Can't allcate memory for eraseinfo"
					" data.\n");
				goto out;
			}
		}
		if ((fd = open(SPLITTING_DUMPFILE(i), O_RDONLY)) < 0) {
			ERRMSG("Can't open a file(%s). %s\n",
			    SPLITTING_DUMPFILE(i), strerror(errno));
			goto out;
		}
		if (lseek(fd, SPLITTING_OFFSET_EI(i), SEEK_SET) < 0) {
			ERRMSG("Can't seek a file(%s). %s\n",
			    SPLITTING_DUMPFILE(i), strerror(errno));
			goto out;
		}
		if (read(fd, data, SPLITTING_SIZE_EI(i)) !=
						SPLITTING_SIZE_EI(i)) {
			ERRMSG("Can't read a file(%s). %s\n",
			    SPLITTING_DUMPFILE(i), strerror(errno));
			goto out;
		}
		if (!write_cache(&cd_data, data, SPLITTING_SIZE_EI(i)))
			goto out;
		size_eraseinfo += SPLITTING_SIZE_EI(i);

		close(fd);
		fd = 0;
	}
	if (size_eraseinfo) {
		if (!write_cache_bufsz(&cd_data))
			goto out;

		if (!update_eraseinfo_of_sub_header(offset_eraseinfo,
						    size_eraseinfo))
			goto out;
	}
	print_progress(PROGRESS_COPY, num_dumpable, num_dumpable);
	print_execution_time(PROGRESS_COPY, &tv_start);

	ret = TRUE;
out:
	free_cache_data(&cd_pd);
	free_cache_data(&cd_data);

	if (data)
		free(data);
	if (fd > 0)
		close(fd);

	return ret;
}

int
reassemble_dumpfile(void)
{
	if (!get_splitting_info())
		return FALSE;

	if (!open_dump_bitmap())
		return FALSE;

	if (!open_dump_file())
		return FALSE;

	if (!reassemble_kdump_header())
		return FALSE;

	if (!reassemble_kdump_pages())
		return FALSE;

	close_dump_file();
	close_dump_bitmap();

	return TRUE;
}

int
check_param_for_generating_vmcoreinfo(int argc, char *argv[])
{
	if (argc != optind)
		return FALSE;

	if (info->flag_compress        || info->dump_level
	    || info->flag_elf_dumpfile || info->flag_read_vmcoreinfo
	    || info->flag_flatten      || info->flag_rearrange
	    || info->flag_exclude_xen_dom
	    || (!info->name_vmlinux && !info->name_xen_syms))

		return FALSE;

	return TRUE;
}

/*
 * Parameters for creating dumpfile from the dump data
 * of flattened format by rearranging the dump data.
 */
int
check_param_for_rearranging_dumpdata(int argc, char *argv[])
{
	if (argc != optind + 1)
		return FALSE;

	if (info->flag_compress        || info->dump_level
	    || info->flag_elf_dumpfile || info->flag_read_vmcoreinfo
	    || info->name_vmlinux      || info->name_xen_syms
	    || info->flag_flatten      || info->flag_generate_vmcoreinfo
	    || info->flag_exclude_xen_dom)
		return FALSE;

	info->name_dumpfile = argv[optind];
	return TRUE;
}

/*
 * Parameters for reassembling multiple dumpfiles into one dumpfile.
 */
int
check_param_for_reassembling_dumpfile(int argc, char *argv[])
{
	int i;

	info->num_dumpfile  = argc - optind - 1;
	info->name_dumpfile = argv[argc - 1];

	DEBUG_MSG("num_dumpfile : %d\n", info->num_dumpfile);

	if (info->flag_compress        || info->dump_level
	    || info->flag_elf_dumpfile || info->flag_read_vmcoreinfo
	    || info->name_vmlinux      || info->name_xen_syms
	    || info->flag_flatten      || info->flag_generate_vmcoreinfo
	    || info->flag_exclude_xen_dom || info->flag_split)
		return FALSE;

	if ((info->splitting_info
	    = malloc(sizeof(splitting_info_t) * info->num_dumpfile))
	    == NULL) {
		MSG("Can't allocate memory for splitting_info.\n");
		return FALSE;
	}
	for (i = 0; i < info->num_dumpfile; i++)
		SPLITTING_DUMPFILE(i) = argv[optind + i];

	return TRUE;
}

/*
 * Check parameters to create the dump file.
 */
int
check_param_for_creating_dumpfile(int argc, char *argv[])
{
	int i;

	if (info->flag_generate_vmcoreinfo || info->flag_rearrange)
		return FALSE;

	if ((message_level < MIN_MSG_LEVEL)
	    || (MAX_MSG_LEVEL < message_level)) {
		message_level = DEFAULT_MSG_LEVEL;
		MSG("Message_level is invalid.\n");
		return FALSE;
	}
	if ((info->flag_compress && info->flag_elf_dumpfile)
	    || (info->flag_read_vmcoreinfo && info->name_vmlinux)
	    || (info->flag_read_vmcoreinfo && info->name_xen_syms))
		return FALSE;

	if (info->flag_flatten && info->flag_split)
		return FALSE;

	if (info->name_filterconfig && !info->name_vmlinux)
		return FALSE;

	if ((argc == optind + 2) && !info->flag_flatten
				 && !info->flag_split) {
		/*
		 * Parameters for creating the dumpfile from vmcore.
		 */
		info->name_memory   = argv[optind];
		info->name_dumpfile = argv[optind+1];

	} else if ((argc > optind + 2) && info->flag_split) {
		/*
		 * Parameters for creating multiple dumpfiles from vmcore.
		 */
		info->num_dumpfile = argc - optind - 1;
		info->name_memory  = argv[optind];

		if (info->flag_elf_dumpfile) {
			MSG("Options for splitting dumpfile cannot be used with Elf format.\n");
			return FALSE;
		}
		if ((info->splitting_info
		    = malloc(sizeof(splitting_info_t) * info->num_dumpfile))
		    == NULL) {
			MSG("Can't allocate memory for splitting_info.\n");
			return FALSE;
		}
		for (i = 0; i < info->num_dumpfile; i++)
			SPLITTING_DUMPFILE(i) = argv[optind + 1 + i];

	} else if ((argc == optind + 1) && info->flag_flatten) {
		/*
		 * Parameters for outputting the dump data of the
		 * flattened format to STDOUT.
		 */
		info->name_memory   = argv[optind];

	} else
		return FALSE;

	return TRUE;
}

int
parse_dump_level(char *str_dump_level)
{
	int i, ret = FALSE;
	char *buf, *ptr;

	if (!(buf = strdup(str_dump_level))) {
		MSG("Can't duplicate strings(%s).\n", str_dump_level);
		return FALSE;
	}
	info->max_dump_level = 0;
	info->num_dump_level = 0;
	ptr = buf;
	while(TRUE) {
		ptr = strtok(ptr, ",");
		if (!ptr)
			break;

		i = atoi(ptr);
		if ((i < MIN_DUMP_LEVEL) || (MAX_DUMP_LEVEL < i)) {
			MSG("Dump_level(%d) is invalid.\n", i);
			goto out;
		}
		if (NUM_ARRAY_DUMP_LEVEL <= info->num_dump_level) {
			MSG("Dump_level is invalid.\n");
			goto out;
		}
		if (info->max_dump_level < i)
			info->max_dump_level = i;
		if (info->num_dump_level == 0)
			info->dump_level = i;
		info->array_dump_level[info->num_dump_level] = i;
		info->num_dump_level++;
		ptr = NULL;
	}
	ret = TRUE;
out:
	free(buf);

	return ret;
}

static struct option longopts[] = {
	{"split", no_argument, NULL, 's'}, 
	{"reassemble", no_argument, NULL, 'r'},
	{"xen-syms", required_argument, NULL, 'y'},
	{"xen-vmcoreinfo", required_argument, NULL, 'z'},
	{"xen_phys_start", required_argument, NULL, 'P'},
	{"message-level", required_argument, NULL, 'm'},
	{"vtop", required_argument, NULL, 'V'},
	{"dump-dmesg", no_argument, NULL, 'M'}, 
	{"config", required_argument, NULL, 'C'},
	{"help", no_argument, NULL, 'h'},
	{0, 0, 0, 0}
};

int
main(int argc, char *argv[])
{
	int i, opt, flag_debug = FALSE;

	if ((info = calloc(1, sizeof(struct DumpInfo))) == NULL) {
		ERRMSG("Can't allocate memory for the pagedesc cache. %s.\n",
		    strerror(errno));
		goto out;
	}
	if ((info->dump_header = calloc(1, sizeof(struct disk_dump_header)))
	    == NULL) {
		ERRMSG("Can't allocate memory for the dump header. %s\n",
		    strerror(errno));
		goto out;
	}
	initialize_tables();

	info->block_order = DEFAULT_ORDER;
	message_level = DEFAULT_MSG_LEVEL;
	while ((opt = getopt_long(argc, argv, "b:cDd:EFfg:hi:MRrsvXx:", longopts,
	    NULL)) != -1) {
		switch (opt) {
		case 'b':
			info->block_order = atoi(optarg);
			break;
		case 'C':
			info->name_filterconfig = optarg;
			break;
		case 'c':
			info->flag_compress = 1;
			break;
		case 'D':
			flag_debug = TRUE;
			break;
		case 'd':
			if (!parse_dump_level(optarg))
				goto out;
			break;
		case 'E':
			info->flag_elf_dumpfile = 1;
			break;
		case 'F':
			info->flag_flatten = 1;
			/*
			 * All messages are output to STDERR because STDOUT is
			 * used for outputting dump data.
			 */
			flag_strerr_message = TRUE;
			break;
		case 'f':
			info->flag_force = 1;
			break;
		case 'g':
			info->flag_generate_vmcoreinfo = 1;
			info->name_vmcoreinfo = optarg;
			break;
		case 'h':
			info->flag_show_usage = 1;
			break;
		case 'i':
			info->flag_read_vmcoreinfo = 1;
			info->name_vmcoreinfo = optarg;
			break;
		case 'm':
			message_level = atoi(optarg);
			break;
		case 'M':
			info->flag_dmesg = 1;
			break;
		case 'P':
			info->xen_phys_start = strtoul(optarg, NULL, 0);
			break;
		case 'R':
			info->flag_rearrange = 1;
			break;
		case 's':
			info->flag_split = 1;
			break;
		case 'r':
			info->flag_reassemble = 1;
			break;
		case 'V':
			info->vaddr_for_vtop = strtoul(optarg, NULL, 0);
			break;
		case 'v':
			info->flag_show_version = 1;
			break;
		case 'X':
			info->flag_exclude_xen_dom = 1;
			break;
		case 'x':
			info->name_vmlinux = optarg;
			break;
		case 'y':
			info->name_xen_syms = optarg;
			break;
		case 'z':
			info->flag_read_vmcoreinfo = 1;
			info->name_vmcoreinfo = optarg;
			break;
		case '?':
			MSG("Commandline parameter is invalid.\n");
			MSG("Try `makedumpfile --help' for more information.\n");
			goto out;
		}
	}
	if (flag_debug)
		message_level |= ML_PRINT_DEBUG_MSG;

	if (info->flag_show_usage) {
		print_usage();
		return COMPLETED;
	}
	if (info->flag_show_version) {
		show_version();
		return COMPLETED;
	}

	if (elf_version(EV_CURRENT) == EV_NONE ) {
		/*
		 * library out of date
		 */
		ERRMSG("Elf library out of date!\n");
		goto out;
	}
	if (info->flag_generate_vmcoreinfo) {
		if (!check_param_for_generating_vmcoreinfo(argc, argv)) {
			MSG("Commandline parameter is invalid.\n");
			MSG("Try `makedumpfile --help' for more information.\n");
			goto out;
		}
		if (!open_files_for_generating_vmcoreinfo())
			goto out;

		if (info->name_xen_syms) {
			if (!generate_vmcoreinfo_xen())
				goto out;
		} else {
			if (!generate_vmcoreinfo())
				goto out;
		}

		if (!close_files_for_generating_vmcoreinfo())
			goto out;

		MSG("\n");
		MSG("The vmcoreinfo is saved to %s.\n", info->name_vmcoreinfo);

	} else if (info->flag_rearrange) {
		if (!check_param_for_rearranging_dumpdata(argc, argv)) {
			MSG("Commandline parameter is invalid.\n");
			MSG("Try `makedumpfile --help' for more information.\n");
			goto out;
		}
		if (!open_files_for_rearranging_dumpdata())
			goto out;

		if (!rearrange_dumpdata())
			goto out;

		if (!close_files_for_rearranging_dumpdata())
			goto out;

		MSG("\n");
		MSG("The dumpfile is saved to %s.\n", info->name_dumpfile);
	} else if (info->flag_reassemble) {
		if (!check_param_for_reassembling_dumpfile(argc, argv)) {
			MSG("Commandline parameter is invalid.\n");
			MSG("Try `makedumpfile --help' for more information.\n");
			goto out;
		}
		if (!reassemble_dumpfile())
			goto out;

		MSG("\n");
		MSG("The dumpfile is saved to %s.\n", info->name_dumpfile);
	} else if (info->flag_dmesg) {
		if (!check_param_for_creating_dumpfile(argc, argv)) {
			MSG("Commandline parameter is invalid.\n");
			MSG("Try `makedumpfile --help' for more information.\n");
			goto out;
		}
		if (!dump_dmesg())
			goto out;

		MSG("\n");
		MSG("The dmesg log is saved to %s.\n", info->name_dumpfile);
	} else {
		if (!check_param_for_creating_dumpfile(argc, argv)) {
			MSG("Commandline parameter is invalid.\n");
			MSG("Try `makedumpfile --help' for more information.\n");
			goto out;
		}
		if (!create_dumpfile())
			goto out;

		MSG("\n");
		if (info->flag_split) {
			MSG("The dumpfiles are saved to ");
			for (i = 0; i < info->num_dumpfile; i++) {
				if (i != (info->num_dumpfile - 1))
					MSG("%s, ", SPLITTING_DUMPFILE(i));
				else
					MSG("and %s.\n", SPLITTING_DUMPFILE(i));
			}
		} else {
			MSG("The dumpfile is saved to %s.\n", info->name_dumpfile);
		}
	}
	retcd = COMPLETED;
out:
	MSG("\n");
	if (retcd == COMPLETED)
		MSG("makedumpfile Completed.\n");
	else
		MSG("makedumpfile Failed.\n");

	if (info) {
		if (info->dh_memory)
			free(info->dh_memory);
		if (info->kh_memory)
			free(info->kh_memory);
		if (info->valid_pages)
			free(info->valid_pages);
		if (info->bitmap_memory)
			free(info->bitmap_memory);
		if (info->fd_memory)
			close(info->fd_memory);
		if (info->fd_dumpfile)
			close(info->fd_dumpfile);
		if (info->fd_bitmap)
			close(info->fd_bitmap);
		if (vt.node_online_map != NULL)
			free(vt.node_online_map);
		if (info->mem_map_data != NULL)
			free(info->mem_map_data);
		if (info->dump_header != NULL)
			free(info->dump_header);
		if (info->splitting_info != NULL)
			free(info->splitting_info);
		if (info->p2m_mfn_frame_list != NULL)
			free(info->p2m_mfn_frame_list);
		free(info);
	}
	free_elf_info();

	return retcd;
}
