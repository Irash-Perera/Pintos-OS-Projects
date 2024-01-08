#include "userprog/syscall.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <stdlib.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "list.h"
#include "devices/shutdown.h"
#include "devices/block.h"

static void syscall_handler(struct intr_frame *);
static struct lock file_lock; // A lock for file operations

// Helper functions
bool isValidPointer(const void *pointer); // Checks if a pointer is valid
static bool create_new_file(const char *file_name, unsigned file_size); // Creates a new file
static int open_file(const char *file); // Opens a file
void exit(int status); // Exits the current process
static int write_to_file(int fd, const void *buffer, unsigned size); // Writes to a file
struct file_details *get_open_file_details(int fd); // Retrieves details of an open file
static int read_file(int fd, const void *buffer, unsigned size); // Reads from a file
void close_file(int fd); // Closes an open file

// Structure to store information about open files
struct file_details {
  int fd;           // File descriptor
  struct file *cur_file; // Pointer to the file structure
  struct list_elem elem; // List element for file list
};

void syscall_init(void) {
  // Register the syscall_handler to be called when a syscall interrupt occurs
  intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init(&file_lock); // Initialize the file_lock
}

static void syscall_handler(struct intr_frame *f UNUSED) {
  // Extract syscall arguments from the stack
  int *data_pointer = f->esp;
  uint32_t *argv0 = data_pointer + 1;
  uint32_t *argv1 = data_pointer + 2;
  uint32_t *argv2 = data_pointer + 3;

  // Check if the pointers and syscall number are valid
  if (!isValidPointer(data_pointer) || !isValidPointer(argv0) || !isValidPointer(argv1) || !isValidPointer(argv2)) {
    exit(-1);
  }

  int systemCall = *(int *)f->esp; // Get the syscall number

  struct file_details *currentFileDet;

  switch (systemCall) {
    case SYS_HALT:
      shutdown_power_off();
      break;

    case SYS_EXIT:
      exit(*argv0); // Terminate the current process with the given status
      break;

    case SYS_EXEC:
      lock_acquire(&file_lock);
      f->eax = process_execute((char *)*argv0); // Execute a new process
      lock_release(&file_lock);
      break;

    case SYS_WAIT:
      f->eax = process_wait(*argv0); // Wait for a child process to terminate
      break;

    case SYS_CREATE:
      f->eax = create_new_file((char *)*argv0, *argv1); // Create a new file
      break;

    case SYS_REMOVE:
      lock_acquire(&file_lock);
      f->eax = filesys_remove((char *)*argv0); // Remove a file
      lock_release(&file_lock);
      break;

    case SYS_OPEN:
      f->eax = open_file((char *)*argv0); // Open a file
      break;

    case SYS_FILESIZE:
      currentFileDet = get_open_file_details(*argv0);
      lock_acquire(&file_lock);
      if (currentFileDet != NULL) {
        f->eax = file_length(currentFileDet->cur_file); // Get the size of an open file
      } else {
        f->eax = -1;
      }
      lock_release(&file_lock);
      break;

    case SYS_READ:
      f->eax = read_file(*argv0, (void *)*argv1, *argv2); // Read from a file or console
      break;

    case SYS_WRITE:
      f->eax = write_to_file(*argv0, (void *)*argv1, *argv2); // Write to a file or console
      break;

    case SYS_SEEK:
      currentFileDet = get_open_file_details(*argv0);
      if (currentFileDet != NULL) {
        lock_acquire(&file_lock);
        file_seek(currentFileDet->cur_file, *((unsigned *)argv1)); // Change the position in a file
        lock_release(&file_lock);
      }
      break;

    case SYS_TELL:
      currentFileDet = get_open_file_details(*((int *)argv0));
      if (currentFileDet == NULL) {
        f->eax = -1;
      } else {
        lock_acquire(&file_lock);
        f->eax = file_tell(currentFileDet->cur_file); // Get the current position in a file
        lock_release(&file_lock);
      }
      break;

    case SYS_CLOSE:
      close_file(*argv0); // Close an open file
      break;

    default:
      break;
  }
}

// Check if a pointer is valid and in user space
bool isValidPointer(const void *pointer) {
  if (pointer == NULL) {
    return false;
  }

  if (!is_user_vaddr(pointer) || !pagedir_get_page(thread_current()->pagedir, pointer)) {
    return false;
  }

  return true;
}

// Create a new file with a given name and size
static bool create_new_file(const char *file_name, unsigned file_size) {
  if (!isValidPointer(file_name)) {
    exit(-1);
  }

  lock_acquire(&file_lock);

  bool status = filesys_create(file_name, file_size);

  lock_release(&file_lock);

  return status;
}

// Open a file with the given name
static int open_file(const char *file) {
  int fd = -1;

  if (!isValidPointer(file))
    exit(-1);

  lock_acquire(&file_lock);

  struct list *open_file_list = &thread_current()->files;

  struct file *file_struct = filesys_open(file);

  if (file_struct != NULL) {
    struct file_details *currentFileDet = malloc(sizeof(struct file_details));
    currentFileDet->fd = thread_current()->fd_count;
    thread_current()->fd_count++;
    currentFileDet->cur_file = file_struct;
    fd = currentFileDet->fd;

    list_push_back(&thread_current()->files, &currentFileDet->elem);
  }

  lock_release(&file_lock);

  return fd;
}

// Read from a file or console
static int read_file(int fd, const void *buffer, unsigned size) {
  if (!isValidPointer(buffer))
    exit(-1);

  if (!isValidPointer(buffer + size - 1))
    exit(-1);

  int ret = -1;

  if (fd == 0) {
    uint8_t *bp = buffer;
    uint8_t c;
    unsigned int cnt;
    for (int i = 0; i < size; i++) {
      c = input_getc();
      if (c == 0)
        break;
      bp[i] = c;
      cnt++;
    }
    bp++;
    *bp = 0;

    ret = size - cnt;
  } else {
    struct file_details *currentFileDet = get_open_file_details(fd);
    if (currentFileDet != NULL) {
      lock_acquire(&file_lock);
      ret = file_read(currentFileDet->cur_file, buffer, size);
      lock_release(&file_lock);
    }
  }

  return ret;
}

// Write to a file or console
static int write_to_file(int fd, const void *buffer, unsigned size) {
  if (buffer == NULL)
    exit(-1);

  if (!isValidPointer(buffer))
    exit(-1);

  if (!isValidPointer(buffer + size - 1))
    exit(-1);

  lock_acquire(&file_lock);

  int status = 0;

  if (fd == 1) {
    putbuf(buffer, size);
    status = size;
  } else {
    struct file_details *currentFileDet = get_open_file_details(fd);

    if (currentFileDet != NULL) {
      status = file_write(currentFileDet->cur_file, buffer, size);
    }
  }

  lock_release(&file_lock);

  return status;
}

// Close an open file
void close_file(int fd) {
  struct list_elem *e;
  struct list files = thread_current()->files;

  for (e = list_begin(&files); e != list_end(&files); e = list_next(e)) {
    struct file_details *currentFileDet = list_entry(e, struct file_details, elem);

    if (currentFileDet->fd == fd) {
      lock_acquire(&file_lock);
      file_close(currentFileDet->cur_file); // Close the file
      list_remove(&currentFileDet->elem); // Remove the file details from the list
      lock_release(&file_lock);
      break;
    }
  }
}

// Exit the current process with the given status
void exit(int status) {
  printf("%s: exit(%d)\n", thread_current()->name, status);

  thread_current()->parent->ex = true; // Set the exit status in the parent process
  thread_current()->exit_code = status; // Set the exit code for the current process
  thread_exit(); // Terminate the current process
}

// Retrieve details of an open file with a given file descriptor
struct file_details *get_open_file_details(int fd) {
  struct list_elem *e;
  struct list files = thread_current()->files;

  for (e = list_begin(&files); e != list_end(&files); e = list_next(e)) {
    struct file_details *currentFileDet = list_entry(e, struct file_details, elem);

    if (currentFileDet->fd == fd) {
      return currentFileDet; // Return the details of the open file
    }
  }

  return NULL; // File with the given file descriptor is not found
}
