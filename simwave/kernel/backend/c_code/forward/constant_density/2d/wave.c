#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <omp.h>

// use single (float) or double precision
// according to the value passed in the compilation cmd
#if defined(FLOAT)
   typedef float f_type;
#elif defined(DOUBLE)
   typedef double f_type;
#endif

// forward_2D_constant_density
double forward(f_type *u, f_type *velocity, f_type *damp,
               f_type *wavelet, size_t wavelet_size, size_t wavelet_count,
               f_type *coeff, size_t *boundary_conditions,
               size_t *src_points_interval, size_t src_points_interval_size,
               f_type *src_points_values, size_t src_points_values_size,
               size_t *src_points_values_offset,
               size_t *rec_points_interval, size_t rec_points_interval_size,
               f_type *rec_points_values, size_t rec_points_values_size,
               size_t *rec_points_values_offset,
               f_type *receivers, size_t num_sources, size_t num_receivers,
               size_t nz, size_t nx, f_type dz, f_type dx,
               size_t saving_stride, f_type dt,
               size_t begin_timestep, size_t end_timestep,
               size_t space_order, size_t num_snapshots){

    size_t stencil_radius = space_order / 2;

    size_t domain_size = nz * nx;

    f_type dzSquared = dz * dz;
    f_type dxSquared = dx * dx;
    f_type dtSquared = dt * dt;

    // timestep pointers
    size_t prev_t = 0;
    size_t current_t = 1;
    size_t next_t = 2;

    // variable to measure execution time
    struct timeval time_start;
    struct timeval time_end;

    // get the start time
    gettimeofday(&time_start, NULL);

    #ifdef GPU_OPENMP
    size_t shot_record_size = wavelet_size * num_receivers;
    size_t u_size = num_snapshots * domain_size;

    #pragma omp target enter data map(to: u[:u_size])
    #pragma omp target enter data map(to: velocity[:domain_size])
    #pragma omp target enter data map(to: damp[:domain_size])
    #pragma omp target enter data map(to: coeff[:stencil_radius+1])
    #pragma omp target enter data map(to: src_points_interval[:src_points_interval_size])
    #pragma omp target enter data map(to: src_points_values[:src_points_values_size])
    #pragma omp target enter data map(to: src_points_values_offset[:num_sources])
    #pragma omp target enter data map(to: rec_points_interval[:rec_points_interval_size])
    #pragma omp target enter data map(to: rec_points_values[:rec_points_values_size])
    #pragma omp target enter data map(to: rec_points_values_offset[:num_receivers])
    #pragma omp target enter data map(to: wavelet[:wavelet_size * wavelet_count])
    #pragma omp target enter data map(to: receivers[:shot_record_size])
    #endif

    // wavefield modeling
    for(size_t n = begin_timestep; n <= end_timestep; n++) {

        // no saving case
        if(saving_stride == 0){
            prev_t = (n - 1) % 3;
            current_t = n % 3;
            next_t = (n + 1) % 3;
        }else{
            // all timesteps saving case
            if(saving_stride == 1){
                prev_t = n - 1;
                current_t = n;
                next_t = n + 1;
            }
        }

        /*
            Section 1: update the wavefield according to the acoustic wave equation
        */

        #ifdef CPU_OPENMP
        #pragma omp parallel for simd
        #endif

        #ifdef GPU_OPENMP
        #pragma omp target teams distribute parallel for collapse(2)
        #endif

        for(size_t i = stencil_radius; i < nz - stencil_radius; i++) {
            for(size_t j = stencil_radius; j < nx - stencil_radius; j++) {
                // index of the current point in the grid
                size_t domain_offset = i * nx + j;

                size_t prev_snapshot = prev_t * domain_size + domain_offset;
                size_t current_snapshot = current_t * domain_size + domain_offset;
                size_t next_snapshot = next_t * domain_size + domain_offset;

                // stencil code to update grid
                f_type value = 0.0;

                f_type sum_x = coeff[0] * u[current_snapshot];
                f_type sum_z = coeff[0] * u[current_snapshot];

                // radius of the stencil
                for(int ir = 1; ir <= stencil_radius; ir++){
                    //neighbors in the horizontal direction
                    sum_x += coeff[ir] * (u[current_snapshot + ir] + u[current_snapshot - ir]);

                    //neighbors in the vertical direction
                    sum_z += coeff[ir] * (u[current_snapshot + (ir * nx)] + u[current_snapshot - (ir * nx)]);
                }

                value += sum_x/dxSquared + sum_z/dzSquared;

                //denominator with damp coefficient
                f_type denominator = (1.0 + damp[domain_offset] * dt);

                value *= (dtSquared * velocity[domain_offset] * velocity[domain_offset]) / denominator;

                u[next_snapshot] = 2.0 / denominator * u[current_snapshot] - ((1.0 - damp[domain_offset] * dt) / denominator) * u[prev_snapshot] + value;
            }
        }

        /*
            Section 2: add the source term
        */

        #ifdef CPU_OPENMP
        #pragma omp parallel for
        #endif

        #ifdef GPU_OPENMP
        #pragma omp target teams distribute parallel for
        #endif

        // for each source
        for(size_t src = 0; src < num_sources; src++){

            size_t wavelet_offset = n - 1;

            if(wavelet_count > 1){
                wavelet_offset = (n-1) * num_sources + src;
            }

            if(wavelet[wavelet_offset] != 0.0){

                // each source has 4 (z_b, z_e, x_b, x_e) point intervals
                size_t offset_src = src * 4;

                // interval of grid points of the source in the Z axis
                size_t src_z_begin = src_points_interval[offset_src + 0];
                size_t src_z_end = src_points_interval[offset_src + 1];

                // interval of grid points of the source in the X axis
                size_t src_x_begin = src_points_interval[offset_src + 2];
                size_t src_x_end = src_points_interval[offset_src + 3];

                // number of grid points of the source in each axis
                size_t src_z_num_points = src_z_end - src_z_begin + 1;
                //size_t src_x_num_points = src_x_end - src_x_begin + 1;

                // pointer to src value offset
                size_t offset_src_kws_index_z = src_points_values_offset[src];

                // index of the Kaiser windowed sinc value of the source point
                size_t kws_index_z = offset_src_kws_index_z;

                // for each source point in the Z axis
                for(size_t i = src_z_begin; i <= src_z_end; i++){
                    size_t kws_index_x = offset_src_kws_index_z + src_z_num_points;

                    // for each source point in the X axis
                    for(size_t j = src_x_begin; j <= src_x_end; j++){

                        f_type kws = src_points_values[kws_index_z] * src_points_values[kws_index_x];

                        // current source point in the grid
                        size_t domain_offset = i * nx + j;
                        size_t next_snapshot = next_t * domain_size + domain_offset;

                        f_type value = dtSquared * velocity[domain_offset] * velocity[domain_offset] * kws * wavelet[wavelet_offset];

                        #if defined(CPU_OPENMP) || defined(GPU_OPENMP)
                        #pragma omp atomic
                        #endif
                        u[next_snapshot] += value;

                        kws_index_x++;
                    }
                    kws_index_z++;
                }
            }
        }


        /*
            Section 3: add boundary conditions (z_before, z_after, x_before, x_after)
            0 - no boundary condition
            1 - null dirichlet
            2 - null neumann
        */
        size_t z_before = boundary_conditions[0];
        size_t z_after = boundary_conditions[1];
        size_t x_before = boundary_conditions[2];
        size_t x_after = boundary_conditions[3];

        // boundary conditions on the left and right (fixed in X)
        #ifdef CPU_OPENMP
        #pragma omp parallel for simd
        #endif

        #ifdef GPU_OPENMP
        #pragma omp target teams distribute parallel for
        #endif
        for(size_t i = stencil_radius; i < nz - stencil_radius; i++){

            // null dirichlet on the left
            if(x_before == 1){
                size_t domain_offset = i * nx + stencil_radius;
                size_t next_snapshot = next_t * domain_size + domain_offset;
                u[next_snapshot] = 0.0;
            }

            // null neumann on the left
            if(x_before == 2){
                for(int ir = 1; ir <= stencil_radius; ir++){
                    size_t domain_offset = i * nx + stencil_radius;
                    size_t next_snapshot = next_t * domain_size + domain_offset;
                    u[next_snapshot - ir] = u[next_snapshot + ir];
                }
            }

            // null dirichlet on the right
            if(x_after == 1){
                size_t domain_offset = i * nx + (nx - stencil_radius - 1);
                size_t next_snapshot = next_t * domain_size + domain_offset;
                u[next_snapshot] = 0.0;
            }

            // null neumann on the right
            if(x_after == 2){
                for(int ir = 1; ir <= stencil_radius; ir++){
                    size_t domain_offset = i * nx + (nx - stencil_radius - 1);
                    size_t next_snapshot = next_t * domain_size + domain_offset;
                    u[next_snapshot + ir] = u[next_snapshot - ir];
                }
            }

        }

        // boundary conditions on the top and bottom (fixed in Z)
        #ifdef CPU_OPENMP
        #pragma omp parallel for simd
        #endif

        #ifdef GPU_OPENMP
        #pragma omp target teams distribute parallel for
        #endif
        for(size_t j = stencil_radius; j < nx - stencil_radius; j++){

            // null dirichlet on the top
            if(z_before == 1){
                size_t domain_offset = stencil_radius * nx + j;
                size_t next_snapshot = next_t * domain_size + domain_offset;
                u[next_snapshot] = 0.0;
            }

            // null neumann on the top
            if(z_before == 2){
                for(int ir = 1; ir <= stencil_radius; ir++){
                    size_t domain_offset = stencil_radius * nx + j;
                    size_t next_snapshot = next_t * domain_size + domain_offset;
                    u[next_snapshot - (ir * nx)] = u[next_snapshot + (ir * nx)];
                }
            }

            // null dirichlet on the bottom
            if(z_after == 1){
                size_t domain_offset = (nz - stencil_radius - 1) * nx + j;
                size_t next_snapshot = next_t * domain_size + domain_offset;
                u[next_snapshot] = 0.0;
            }

            // null neumann on the bottom
            if(z_after == 2){
                for(int ir = 1; ir <= stencil_radius; ir++){
                    size_t domain_offset = (nz - stencil_radius - 1) * nx + j;
                    size_t next_snapshot = next_t * domain_size + domain_offset;
                    u[next_snapshot + (ir * nx)] = u[next_snapshot - (ir * nx)];
                }
            }

        }

        /*
            Section 4: compute the receivers
        */

        #ifdef CPU_OPENMP
        #pragma omp parallel for simd
        #endif

        #ifdef GPU_OPENMP
        #pragma omp target teams distribute parallel for
        #endif

        // for each receiver
        for(size_t rec = 0; rec < num_receivers; rec++){

            f_type sum = 0.0;

            // each receiver has 4 (z_b, z_e, x_b, x_e) point intervals
            size_t offset_rec = rec * 4;

            // interval of grid points of the receiver in the Z axis
            size_t rec_z_begin = rec_points_interval[offset_rec + 0];
            size_t rec_z_end = rec_points_interval[offset_rec + 1];

            // interval of grid points of the receiver in the X axis
            size_t rec_x_begin = rec_points_interval[offset_rec + 2];
            size_t rec_x_end = rec_points_interval[offset_rec + 3];

            // number of grid points of the receiver in each axis
            size_t rec_z_num_points = rec_z_end - rec_z_begin + 1;
            //size_t rec_x_num_points = rec_x_end - rec_x_begin + 1;

            // pointer to rec value offset
            size_t offset_rec_kws_index_z = rec_points_values_offset[rec];

            // index of the Kaiser windowed sinc value of the receiver point
            size_t kws_index_z = offset_rec_kws_index_z;

            // for each receiver point in the Z axis
            for(size_t i = rec_z_begin; i <= rec_z_end; i++){
                size_t kws_index_x = offset_rec_kws_index_z + rec_z_num_points;

                // for each receiver point in the X axis
                for(size_t j = rec_x_begin; j <= rec_x_end; j++){

                    f_type kws = rec_points_values[kws_index_z] * rec_points_values[kws_index_x];

                    // current receiver point in the grid
                    size_t domain_offset = i * nx + j;
                    size_t current_snapshot = current_t * domain_size + domain_offset;
                    sum += u[current_snapshot] * kws;

                    kws_index_x++;
                }
                kws_index_z++;
            }

            size_t current_rec_n = (n-1) * num_receivers + rec;
            receivers[current_rec_n] = sum;
        }

        // stride timesteps saving case
        if(saving_stride > 1){
            // shift the pointer
            if(n % saving_stride == 1){

                prev_t = current_t;
                current_t += 1;
                next_t += 1;

                // even stride adjust case
                if(saving_stride % 2 == 0 && n < end_timestep){
                    size_t swap = current_t;
                    current_t = next_t;
                    next_t = swap;

                    #ifdef CPU_OPENMP
                    #pragma omp parallel for
                    #endif

                    #ifdef GPU_OPENMP
                    #pragma omp target teams distribute parallel for
                    #endif

                    // exchange of values ​​required
                    for(size_t i = 0; i < nz; i++) {
                        for(size_t j = 0; j < nx; j++) {
                            // index of the current point in the grid
                            size_t domain_offset = i * nx + j;

                            size_t current_snapshot = current_t * domain_size + domain_offset;
                            size_t next_snapshot = next_t * domain_size + domain_offset;

                            f_type aux = u[current_snapshot];
                            u[current_snapshot] = u[next_snapshot];
                            u[next_snapshot] = aux;
                        }
                    }
                }

            }else{
                prev_t = current_t;
                current_t = next_t;
                next_t = prev_t;
            }
        }

    }

    #ifdef GPU_OPENMP
    #pragma omp target exit data map(from: receivers[:shot_record_size])
    #pragma omp target exit data map(from: u[:u_size])

    #pragma omp target exit data map(delete: velocity[:domain_size])
    #pragma omp target exit data map(delete: damp[:domain_size])
    #pragma omp target exit data map(delete: coeff[:stencil_radius+1])
    #pragma omp target exit data map(delete: src_points_interval[:src_points_interval_size])
    #pragma omp target exit data map(delete: src_points_values[:src_points_values_size])
    #pragma omp target exit data map(delete: src_points_values_offset[:num_sources])
    #pragma omp target exit data map(delete: rec_points_interval[:rec_points_interval_size])
    #pragma omp target exit data map(delete: rec_points_values[:rec_points_values_size])
    #pragma omp target exit data map(delete: rec_points_values_offset[:num_receivers])
    #pragma omp target exit data map(delete: wavelet[:wavelet_size * wavelet_count])
    #pragma omp target exit data map(delete: receivers[:shot_record_size])
    #pragma omp target exit data map(delete: u[:u_size])
    #endif

    // get the end time
    gettimeofday(&time_end, NULL);

    double exec_time = (double) (time_end.tv_sec - time_start.tv_sec) + (double) (time_end.tv_usec - time_start.tv_usec) / 1000000.0;

    return exec_time;
}
