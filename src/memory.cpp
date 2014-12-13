#include "memory.hpp"

namespace vram {
    namespace memory {
        // Connection with OpenCL
        bool ready = false;
        cl::Context context;
        cl::Device device;
        cl::CommandQueue queue;

    #ifdef OPENCL_1_1
        cl::Buffer zero_buffer;
    #endif

        std::vector<cl::Buffer> pool;
        int total_blocks = 0;

        // Fill buffer with zeros
        static int clear_buffer(const cl::Buffer& buf) {
        #ifdef OPENCL_1_1
            return queue.enqueueCopyBuffer(zero_buffer, buf, 0, 0, block::size, nullptr, nullptr);
        #else
            return queue.enqueueFillBuffer(buf, 0, 0, block::size, nullptr, nullptr);
        #endif
        }

        // Find platform with OpenCL capable GPU
        static bool init_opencl() {
            if (ready) return true;

            std::vector<cl::Platform> platforms;
            cl::Platform::get(&platforms);
            if (platforms.size() == 0) return false;

            for (auto& platform : platforms) {
                std::vector<cl::Device> gpu_devices;
                platform.getDevices(CL_DEVICE_TYPE_GPU, &gpu_devices);
                if (gpu_devices.size() == 0) continue;

                device = gpu_devices[0];
                context = cl::Context(gpu_devices);
                queue = cl::CommandQueue(context, device);

            #ifdef OPENCL_1_1
                char zero_data[block::size] = {};
                int r;
                zero_buffer = cl::Buffer(context, CL_MEM_READ_ONLY, block::size, nullptr, &r);
                if (r != CL_SUCCESS) return false;
                r = queue.enqueueWriteBuffer(zero_buffer, true, 0, block::size, zero_data, nullptr, nullptr);
                if (r != CL_SUCCESS) return false;
            #endif

                return true;
            }

            return false;
        }

        // Called for asynchronous writes to clean up the data copy
        static CL_CALLBACK void async_write_dealloc(cl_event, cl_int, void* data) {
            delete [] reinterpret_cast<char*>(data);
        }

        bool is_available() {
            return (ready = init_opencl());
        }

        int pool_size() {
            return total_blocks;
        }

        int pool_available() {
            return pool.size();
        }

        size_t increase_pool(size_t size) {
            int block_count = 1 + (size - 1) / block::size;
            int r;

            for (int i = 0; i < block_count; i++) {
                cl::Buffer buf(context, CL_MEM_READ_WRITE, block::size, nullptr, &r);

                if (r == CL_SUCCESS && clear_buffer(buf) == CL_SUCCESS) {
                    pool.push_back(buf);
                    total_blocks++;
                } else {
                    return i * block::size;
                }
            }

            return block_count * block::size;
        }

        block_ref allocate() {
            if (pool.size() != 0) {
                return block_ref(new block());
            } else {
                return nullptr;
            }
        }

        block::block() {
            buffer = pool.back();
            pool.pop_back();
        }

        block::~block() {
            pool.push_back(buffer);
        }

        void block::read(off_t offset, size_t size, void* data) const {
            if (dirty) {
                memset(data, 0, size);
            } else {
                // Queue is configured for in-order execution, so writes before this
                // are guaranteed to be completed first
                queue.enqueueReadBuffer(buffer, true, offset, size, data, nullptr, nullptr);
            }
        }

        void block::write(off_t offset, size_t size, const void* data, bool async) {
            // If this block has not been written to yet, and this call doesn't
            // overwrite the entire block, clear with zeros first
            if (dirty && size != block::size) {
                clear_buffer(buffer);
            }

            if (async) {
                char* data_copy = new char[size];
                memcpy(data_copy, data, size);
                data = data_copy;
            }

            cl::Event event;
            queue.enqueueWriteBuffer(buffer, !async, offset, size, data, nullptr, &event);

            if (async) {
                event.setCallback(CL_COMPLETE, async_write_dealloc, const_cast<void*>(data));
            }

            last_write = event;
            dirty = false;
        }

        void block::sync() {
            last_write.wait();
        }
    }
}
