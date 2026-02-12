// snap_test.c
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

void print_test_header(char *msg) {
    printf(1, "\n========================================\n");
    printf(1, "%s\n", msg);
    printf(1, "========================================\n");
}

void create_test_file(char *filename, char *content) {
    int fd = open(filename, O_CREATE | O_WRONLY);
    if (fd < 0) {
        printf(2, "Error: cannot create %s\n", filename);
        return;
    }
    write(fd, content, strlen(content));
    close(fd);
    printf(1, "Created file: %s with content: %s\n", filename, content);
}

void read_and_print_file(char *filename) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        printf(1, "File %s does not exist\n", filename);
        return;
    }
    
    char buf[512];
    int n = read(fd, buf, sizeof(buf));
    if (n > 0) {
        buf[n] = 0;
        printf(1, "Content of %s: %s\n", filename, buf);
    }
    close(fd);
}

int main(int argc, char *argv[]) {
    int snap_id1, snap_id2;
    
    print_test_header("TEST 1: Create Initial Snapshot");
    snap_id1 = snapshot_create();
    if (snap_id1 < 0) {
        printf(2, "FAIL: Initial snapshot creation failed\n");
        exit();
    }
    printf(1, "PASS: Snapshot 1 created with ID: %d\n", snap_id1);
    
    print_test_header("TEST 2: Create and Modify Files");
    create_test_file("test1.txt", "Original content in test1\n");
    create_test_file("test2.txt", "Original content in test2\n");
    
    print_test_header("TEST 3: Create Second Snapshot");
    snap_id2 = snapshot_create();
    if (snap_id2 < 0) {
        printf(2, "FAIL: Second snapshot creation failed\n");
        exit();
    }
    printf(1, "PASS: Snapshot 2 created with ID: %d\n", snap_id2);
    
    print_test_header("TEST 4: Modify Files After Snapshot 2");
    int fd = open("test1.txt", O_WRONLY);
    if (fd >= 0) {
        write(fd, "Modified content in test1\n", 27);
        close(fd);
        printf(1, "Modified test1.txt\n");
    }
    
    create_test_file("test3.txt", "New file after snapshot 2\n");
    
    printf(1, "\nCurrent state:\n");
    read_and_print_file("test1.txt");
    read_and_print_file("test2.txt");
    read_and_print_file("test3.txt");
    
    print_test_header("TEST 5: Rollback to Snapshot 2");
    if (snapshot_rollback(snap_id2) < 0) {
        printf(2, "FAIL: Rollback to snapshot 2 failed\n");
        exit();
    }
    printf(1, "PASS: Rolled back to snapshot 2\n");
    
    printf(1, "\nState after rollback to snapshot 2:\n");
    read_and_print_file("test1.txt");  // Should be "Original content"
    read_and_print_file("test2.txt");  // Should exist
    read_and_print_file("test3.txt");  // Should NOT exist
    
    print_test_header("TEST 6: Rollback to Snapshot 1");
    if (snapshot_rollback(snap_id1) < 0) {
        printf(2, "FAIL: Rollback to snapshot 1 failed\n");
        exit();
    }
    printf(1, "PASS: Rolled back to snapshot 1\n");
    
    printf(1, "\nState after rollback to snapshot 1:\n");
    read_and_print_file("test1.txt");  // Should NOT exist
    read_and_print_file("test2.txt");  // Should NOT exist
    
    print_test_header("TEST 7: Delete Snapshot");
    if (snapshot_delete(snap_id1) < 0) {
        printf(2, "FAIL: Delete snapshot 1 failed\n");
        exit();
    }
    printf(1, "PASS: Snapshot 1 deleted\n");
    
    if (snapshot_delete(snap_id2) < 0) {
        printf(2, "FAIL: Delete snapshot 2 failed\n");
        exit();
    }
    printf(1, "PASS: Snapshot 2 deleted\n");
    
    print_test_header("TEST 8: Try Invalid Snapshot ID");
    if (snapshot_rollback(999) >= 0) {
        printf(2, "FAIL: Should fail on invalid snapshot ID\n");
    } else {
        printf(1, "PASS: Correctly rejected invalid snapshot ID\n");
    }
    
    print_test_header("ALL TESTS COMPLETED");
    printf(1, "Check /snapshot/ directory for snapshot contents\n");
    
    exit();
}