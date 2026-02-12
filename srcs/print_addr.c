#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"

int main(int argc, char *argv[]) {
	if (argc != 2) {
		printf(2, "Usage: print_addr filename\n");
		exit();
	}

	uint addrs[NDIRECT + 1];
	if (get_file_addrs(argv[1], addrs) < 0) {
		printf(2, "print_addr: cannot get addresses for %s\n", argv[1]);
		exit();
	}

	//직접블록 출력
	for(int i = 0; i < NDIRECT; i++) {
		if (addrs[i] != 0) {
			printf(1, "addr[%d] : %x\n", i, addrs[i]);
		}
	}

	//간접블록 출력
	uint indirect_addrs[NINDIRECT];
	if (get_indirect_addrs(addrs[NDIRECT], indirect_addrs) >= 0) {
		for(int i = 0; i < NINDIRECT; i++) {
			if (indirect_addrs[i] != 0) {
				printf(1, "addr[12] -> [%d] (bn : %d) : %x\n", i, NDIRECT + i, indirect_addrs[i]);
			}
		}
	}

	exit();
}