Sparse Channel Matrix Tools
===========================

Copyright 2013, NICTA.  See COPYRIGHT for license details.

This package contains both a sparse matrix library, and a number of tools for
analysing large, sparse channel matrices and building them from experimental
data supplied as a list of (integal) "input output" pairs.

As far as possible, the tools operate as filters on streams of samples -
transforming stdin to stdout.  They can thus be straightforwardly
daisy-chained, and sample streams (which are generally very large) can be
compressed with external tools (xz,bzip2,gzip,...).

Be aware that both matrix generation and error simulation are
memory-intensive.  A 0.25% dense 250000 x 65536 matrix occupies ~300MB in
sparse format, but building it may easily require 8GB of RAM.  Likewise,
simulation builds a large number of such matrices, and is parallelised.  To
achieve maximum speedup, you would need 8GB of RAM per core (a medium-sized
simulation can easily to 100GB, and consume 1000h of processor time).

----------------------------------------------------------------

After generated a matrix, do a row average on the matrix

```
./row_average color.cm > color.avg
```

Then, smooth the average, for generating the data trend

```
./smooth 10 < color.avg > color.smooth
```

Basic tools
-----------

* `analyse`

  Reads a sample stream from stdin and prints histogram statistics.
  Generating channel matrix for inputing pairs, and printout the
  horizontal value range for each row inside the matrix.

  ```
  analyse < inputfile
  ```

* `analyse_mat`

  Calculates various statistics for a channel matrix, relevant for
  optimisation.

* `capacity`

  Computes the Shannon capacity of the given channel matrix - the
  greatest mutual information over all input distributions.

* `channel_hist`

  Calculates and prints a 2D histogram of the sample stream on stdin.

* `channel_matrix`

  Constructs a channel matrix from the sample stream given on stdin.

* `confidence_interval.py`

  Uses the output of sample_error to estimate the confidence interval
  for the given observed capacity.

* `drop_samples`

  Drop a fixed number of samples for every input modulation.  can be
  good for drop the value generated via the warm-up session

* `extract_plot`

  Generate a visualisation of the given channel matrix, suitable for
  gnuplot 'with image'.

* `filter_samples`

  Drop malformed and out-of-range samples.  can be good for counter
  overflow, filter out the value for counter overflows

* `row_average`

  Average the rows in the given matrix

* `sample_error`

  Run a Monte Carlo simulation for the given matrix, to approximate the
  distribution of capacities expected if the matrix actually had a given
  small bandwidth.  This is used to generate confidence intervals and
  establish the statistical precision of the experimental results.  run
  the simulation on one matrix

  ```
	sample_error \
    matrix \
    same_sample_number_as_the_matrix_data \
    1000 \
    (num of sub matrices) \
    percision_number(1e-3) \
    1 0 1
  ```

* `sim_max.py`

    Return the largest output of sample_error.  return the largest
    output for CI0

* `smooth`

  Smooth the given vector by averaging samples [n-r,n+r].

* `stride`

  Modify the internal layout of the matrix so that n columns can be
  processed in parallel.  May improve vectorization performance.

* `summarise`

  Present a concise summary of the given sample stream.

Tools for managing an experimental corpus
-----------------------------------------

### Running sequence:

1. `experiment_stats.py`
2. `gen_matrices.py`
3. `find_capacities.sh`
4. `make_plots.sh`

### Tools

* `experiment_stats.py`

  Present summary statistics for a corpus of experimental results,
  stored under the given path.  It decompresses the log data stored in a
  path, and feed that into summarise executable file.  The aim is
  present a the total count, row minimum/maximum value, and the column
  minimum/maximum value.  The %.hist file stores the statistics for
  {input, number of output values}.

  ```
  experiment_stats.py /data/experimental/covert/ -m
  ```

  Save the output files into /data/experimental/analysis.

  Save the output printout into /data/experimental/analysis/stats.

* `find_capacities.sh`

  Calculate the capacity of every matrix in the given corpus.  Finding
  the capacity for all the matrices stored in a directory.  It call
  capacity for caculation, with the matrix and percision number.

  ```
  find_capacities.sh /data/experimental/covert/  1e-n (percision number)
  ```

* `gen_matrices.py`

  Generate all channel matrices for the corpus.  For every .xz log file
  stored in the data path that matches the analysis file names,
  decompress, feed into filter_sampels and channel_matrix. the channel
  matrix is stored into /data/experimental/covert/matrices. One matrix
  is generated for all the log file, thus, the matix file is very very
  big.  All the log files are used to generate one matrix.  run the
  experiment_stats.py before this

  ```
  gen_matrices.py pathto/channel_tools/ /data/experimental/covert/
  ```

* `make_matrix.sh`

  Generate a single matrix.

  ```
  make_matrix.sh \
    /data/experimental/covert \
    chip_name \
    channel_name \
    counter_measure_name \
    time_slice \
    log_count
  ```

* `make_plots.sh`

  Generate all plots.  extrating plots from the channel matrices stored
  in the given direcotry.  the results are stored in the
  /data/experimental/covert/matrices/ *.plot, can be used by gnuplot.

  ```
  make_plots.sh  \
    /data/experimental/covert \
    num_row_in_plot(1024) \
    num_col_in_plot(1024)
  ```

* `partition.py`

  Run Monte Carlo error simulations for matrices.

Test and benchmarking tools
---------------------------

* `mult`

  Measure sparse multiplication speed.

* `mvec`

  Measure underlying arithmetic speed.

* `speed_sparse`

  Measure sparse matrix operation speed.

* `test_hist`

  Test various aspects of the block-sparse histogram library.

* `test_sparse`

  Test the sparse matrix library.

Building
--------

On Linux, just type

```
make
```

and to run the test suite,

```
make test
```

Other platforms are as yet untested, but the package should build with
only minor modifications on any UNIX-like platform.

Packaged Software
-----------------

This package includes the dSFMT pseudorandom number generator of Saito
and Matsumoto.  For licensing details, see dSFMT-src-2.2.1/LICENSE.txt.

Dependencies
------------

You will need to download a copy of
[leakiest](http://www.cs.bham.ac.uk/research/projects/infotools/leakiest/).
