#include "types.h"
#include "user.h"

int main(int argc, char *argv[]) {
	int snap_id;

	if (argc != 2) {
		printf(2, "Usage: snap_rollback snapshot_id\n");
		exit();
	}

	snap_id = atoi(argv[1]);

	if (snapshot_rollback(snap_id) < 0) {
		printf(2, "snap_rollback: failed to rollback to snapshot %d\n", snap_id);
		exit();
	}

	printf(1, "Successfully rolled back to snapshot %d\n", snap_id);
	exit();
}