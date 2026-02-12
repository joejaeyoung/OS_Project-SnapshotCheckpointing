#include "types.h"
#include "user.h"

int main(int argc, char *argv[]) {
	int snap_id;

	snap_id = snapshot_create();

	if (snap_id < 0) {
		printf(2, "snap_create: failed to create snapshot\n");
		exit();
	}

	printf(1, "Snapshot created with ID: %d\n", snap_id);
	exit();
}