////////////////////////////////////////////////////////////////////////////////
// Authors: Mateus Fogaça, Isadora Oliveira and Marcelo Danigno
//
//          (Advisor: Ricardo Reis and Paulo Butzen)
//
// BSD 3-Clause License
//
// Copyright (c) 2020, Federal University of Rio Grande do Sul (UFRGS)
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
////////////////////////////////////////////////////////////////////////////////

#include "PartClusManagerKernel.h"
extern "C" {
#include "main/ChacoWrapper.h"
}
#include <time.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>

#include "MLPart.h"
#include "metis.h"
#include "opendb/db.h"
#include "openroad/Error.hh"

namespace PartClusManager {

// Partition Netlist

void PartClusManagerKernel::runPartitioning()
{
  hypergraph();
  if (_options.getTool() == "mlpart") {
    runMlPart();
  } else if (_options.getTool() == "gpmetis") {
    runGpMetis();
  } else {
    runChaco();
  }
}

void PartClusManagerKernel::runChaco()
{
  std::cout << "\nRunning chaco...\n";

  PartSolutions currentResults;
  currentResults.setToolName(_options.getTool());
  unsigned partitionId = generatePartitionId();
  currentResults.setPartitionId(partitionId);
  currentResults.setNumOfRuns(_options.getSeeds().size());
  std::string evaluationFunction = _options.getEvaluationFunction();

  PartSolutions bestResult;
  bestResult.setToolName(_options.getTool());
  bestResult.setPartitionId(partitionId);
  bestResult.setNumOfRuns(1);
  int firstRun = 0;

  std::vector<int> edgeWeights = _graph.getEdgeWeight();
  std::vector<int> vertexWeights = _graph.getVertexWeight();
  std::vector<int> colIdx = _graph.getColIdx();
  std::vector<int> rowPtr = _graph.getRowPtr();
  int numVertices = vertexWeights.size();
  int numVerticesTotal = vertexWeights.size();
  short highestCurrentPartition = 0;

  int architecture = _options.getArchTopology().size();
  int architectureDims = 1;
  int* mesh_dims = (int*) malloc((unsigned) 3 * sizeof(int));
  if (architecture > 0) {
    std::vector<int> archTopology = _options.getArchTopology();
    for (int i = 0; ((i < architecture) && (i < 3)); i++) {
      mesh_dims[i] = archTopology[i];
      architectureDims = architectureDims * archTopology[i];
    }
  }

  int hypercubeDims
      = (int) (std::log2(((double) (_options.getTargetPartitions()))));

  int numVertCoar = _options.getCoarVertices();

  int refinement = _options.getRefinement();

  int termPropagation = 0;
  if (_options.getTermProp()) {
    termPropagation = 1;
  }

  double inbalance = (double) _options.getBalanceConstraint() / 100;

  double coarRatio = _options.getCoarRatio();

  double cutCost = _options.getCutHopRatio();

  int level = _options.getLevel();

  int partitioningMethod = 1;  // Multi-level KL

  int kWay = 1;  // recursive 2-way

  for (long seed : _options.getSeeds()) {
    auto start = std::chrono::system_clock::now();
    std::time_t startTime = std::chrono::system_clock::to_time_t(start);

    int* starts = (int*) malloc((unsigned) (numVertices + 1) * sizeof(int));
    int* currentIndex = starts;
    for (int pointer : rowPtr) {
      *currentIndex = (pointer);
      currentIndex++;
    }
    *currentIndex = colIdx.size();  // Needed so Chaco can find the end of the
                                    // interval of the last vertex

    int* vweights = (int*) malloc((unsigned) numVertices * sizeof(int));
    currentIndex = vweights;
    for (int weigth : vertexWeights) {
      *currentIndex = weigth;
      currentIndex++;
    }

    int* adjacency = (int*) malloc((unsigned) colIdx.size() * sizeof(int));
    currentIndex = adjacency;
    for (int pointer : colIdx) {
      *currentIndex = (pointer + 1);
      currentIndex++;
    }

    float* eweights = (float*) malloc((unsigned) colIdx.size() * sizeof(float));
    float* currentIndexFloat = eweights;
    for (int weigth : edgeWeights) {
      *currentIndexFloat = weigth;
      currentIndexFloat++;
    }

    short* assigment
        = (short*) malloc((unsigned) numVerticesTotal * sizeof(short));

    int oldTargetPartitions = 0;

    if (_options.getExistingID() > -1) {
      // If a previous solution ID already exists...
      PartSolutions existingResult = _results[_options.getExistingID()];
      unsigned existingBestIdx = existingResult.getBestSolutionIdx();
      const std::vector<unsigned long>& vertexResult
          = existingResult.getAssignment(existingBestIdx);
      // Gets the vertex assignments results from the last ID.
      short* currentIndexShort = assigment;
      for (unsigned long existingPartId : vertexResult) {
        // Apply the Partition IDs to the current assignment vector.
        if (existingPartId > oldTargetPartitions) {
          oldTargetPartitions = existingPartId;
        }
        *currentIndexShort = existingPartId;
        currentIndexShort++;
      }

      partitioningMethod = 7;
      kWay = hypercubeDims;
      oldTargetPartitions = oldTargetPartitions + 1;

      if (architecture) {
        hypercubeDims = (int) (std::log2(((double) (architectureDims))));
        kWay = hypercubeDims;
        if (kWay > 3 || architectureDims < oldTargetPartitions
            || architectureDims % 2 == 1) {
          std::cout << "Graph has too many sets (>8), the number of target "
                       "partitions changed or the architecture is invalid.";
          std::exit(1);
        }
      } else {
        if (kWay > 3 || _options.getTargetPartitions() < oldTargetPartitions) {
          std::cout << "Graph has too many sets (>8) or the number of target "
                       "partitions changed.";
          std::exit(1);
        }
      }
    }

    interface_wrap(
        numVertices, /* number of vertices */
        starts,
        adjacency,
        vweights,
        eweights, /* graph definition for chaco */
        NULL,
        NULL,
        NULL, /* x y z positions for the inertial method, not needed for
                 multi-level KL */
        NULL,
        NULL, /* output assigment name and file, isn't needed because internal
                 methods of PartClusManager are used instead */
        assigment, /* vertex assigment vector. Contains the set that each vector
                      is present on.*/
        architecture,
        hypercubeDims,
        mesh_dims, /* architecture, architecture topology and the hypercube
                      dimensions (number of 2-way divisions) */
        NULL, /* desired set sizes for each set, computed automatically, so it
                 isn't needed */
        partitioningMethod,
        1, /* constants that define the methods used by the partitioner ->
              multi-level KL, KL refinement */
        0,
        numVertCoar,
        kWay, /* disables the eigensolver, number of vertices to coarsen down to
                 and bisection/quadrisection/octasection */
        0.001,
        seed, /* tolerance on eigenvectors (hard-coded, not used) and the seed
               */
        termPropagation,
        inbalance, /* terminal propagation enable and inbalance */
        coarRatio,
        cutCost, /* coarsening ratio and cut to hop cost */
        0,
        refinement,
        level); /* debug text enable, refinement and clustering level to
                   export*/

    std::vector<unsigned long> chacoResult;

    for (int i = 0; i < numVertices; i++) {
      short* currentpointer = assigment + i;
      chacoResult.push_back(*currentpointer);
    }

    auto end = std::chrono::system_clock::now();
    unsigned long runtime
        = std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
              .count();

    currentResults.addAssignment(chacoResult, runtime, seed);
    free(assigment);

    if (_options.getSeeds().size() > 19) {
      if (firstRun <= 0) {
        bestResult.addAssignment(chacoResult, runtime, seed);
        _results.push_back(bestResult);
        computePartitionResult(partitionId, evaluationFunction);
        bestResult = _results.back();
        currentResults.clearAssignments();
      } else {
        currentResults.setNumOfRuns(1);
        currentResults.setPartitionId(partitionId + 1);
        _results.push_back(currentResults);
        computePartitionResult((partitionId + 1), evaluationFunction);
        currentResults = _results.back();
        bool isNewIdBetter = comparePartitionings(
            bestResult, currentResults, evaluationFunction);
        if (isNewIdBetter) {
          _results.pop_back();
          _results.pop_back();
          bestResult.clearAssignments();
          bestResult.addAssignment(chacoResult, runtime, seed);

          bestResult.setBestSolutionIdx(0);
          bestResult.setBestRuntime(runtime);
          bestResult.setBestNumHyperedgeCuts(
              currentResults.getBestNumHyperedgeCuts());
          bestResult.setBestNumTerminals(currentResults.getBestNumTerminals());
          bestResult.setBestHopWeigth(currentResults.getBestHopWeigth());
          bestResult.setBestSetSize(currentResults.getBestSetSize());
          bestResult.setBestSetArea(currentResults.getBestSetArea());

          _results.push_back(bestResult);
          currentResults.clearAssignments();
        } else {
          _results.pop_back();
          currentResults.clearAssignments();
        }
      }
      firstRun = firstRun + 1;
      if (firstRun % 100 == 0) {
        std::cout << "Partitioned graph for " << firstRun << " seeds.\n";
      }
    } else {
      std::cout << "Partitioned graph for seed " << seed << " in " << runtime
                << " ms.\n";
    }
  }

  if (_options.getSeeds().size() <= 19) {
    _results.push_back(currentResults);
    computePartitionResult(partitionId, evaluationFunction);
  }
  free(mesh_dims);

  std::cout << "Chaco run completed. Partition ID = " << partitionId
            << ". Total runs = " << _options.getSeeds().size() << ".\n";
}

void PartClusManagerKernel::runGpMetis()
{
  std::cout << "Running GPMetis...\n";
  PartSolutions currentResults;
  currentResults.setToolName(_options.getTool());
  unsigned partitionId = generatePartitionId();
  currentResults.setPartitionId(partitionId);
  currentResults.setNumOfRuns(_options.getSeeds().size());
  std::string evaluationFunction = _options.getEvaluationFunction();

  PartSolutions bestResult;
  bestResult.setToolName(_options.getTool());
  bestResult.setPartitionId(partitionId);
  bestResult.setNumOfRuns(1);
  int firstRun = 0;

  idx_t edgeCut;
  idx_t nPartitions = _options.getTargetPartitions();
  int numVertices = _graph.getNumVertex();
  int numEdges = _graph.getNumEdges();
  idx_t constraints = 1;
  idx_t options[METIS_NOPTIONS];
  METIS_SetDefaultOptions(options);

  options[METIS_OPTION_PTYPE] = METIS_PTYPE_RB;
  options[METIS_OPTION_OBJTYPE] = METIS_OBJTYPE_CUT;
  options[METIS_OPTION_NUMBERING] = 0;
  options[METIS_OPTION_UFACTOR] = _options.getBalanceConstraint() * 10;

  idx_t* vertexWeights
      = (idx_t*) malloc((unsigned) numVertices * sizeof(idx_t));
  idx_t* rowPtr = (idx_t*) malloc((unsigned) (numVertices + 1) * sizeof(idx_t));
  idx_t* colIdx = (idx_t*) malloc((unsigned) numEdges * sizeof(idx_t));
  idx_t* edgeWeights = (idx_t*) malloc((unsigned) numEdges * sizeof(idx_t));
  for (int i = 0; i < numVertices; i++) {
    vertexWeights[i] = _graph.getVertexWeight(i);
    edgeWeights[i] = _graph.getEdgeWeight(i);
    rowPtr[i] = _graph.getRowPtr(i);
    colIdx[i] = _graph.getColIdx(i);
  }
  rowPtr[numVertices] = numEdges;

  for (int i = numVertices; i < numEdges; i++) {
    edgeWeights[i] = _graph.getEdgeWeight(i);
    colIdx[i] = _graph.getColIdx(i);
  }
  for (int seed : _options.getSeeds()) {
    std::vector<unsigned long> gpmetisResults;
    auto start = std::chrono::system_clock::now();
    std::time_t startTime = std::chrono::system_clock::to_time_t(start);
    options[METIS_OPTION_SEED] = seed;
    idx_t* parts = (idx_t*) malloc((unsigned) numVertices * sizeof(idx_t));

    METIS_PartGraphRecursive(&numVertices,
                             &constraints,
                             rowPtr,
                             colIdx,
                             vertexWeights,
                             NULL,
                             edgeWeights,
                             &nPartitions,
                             NULL,
                             NULL,
                             options,
                             &edgeCut,
                             parts);

    for (int i = 0; i < numVertices; i++)
      gpmetisResults.push_back(parts[i]);
    auto end = std::chrono::system_clock::now();
    unsigned long runtime
        = std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
              .count();

    currentResults.addAssignment(gpmetisResults, runtime, seed);
    free(parts);

    if (_options.getSeeds().size() > 19) {
      if (firstRun <= 0) {
        bestResult.addAssignment(gpmetisResults, runtime, seed);
        _results.push_back(bestResult);
        computePartitionResult(partitionId, evaluationFunction);
        bestResult = _results.back();
        currentResults.clearAssignments();
      } else {
        currentResults.setNumOfRuns(1);
        currentResults.setPartitionId(partitionId + 1);
        _results.push_back(currentResults);
        computePartitionResult((partitionId + 1), evaluationFunction);
        currentResults = _results.back();
        bool isNewIdBetter = comparePartitionings(
            bestResult, currentResults, evaluationFunction);
        if (isNewIdBetter) {
          _results.pop_back();
          _results.pop_back();
          bestResult.clearAssignments();
          bestResult.addAssignment(gpmetisResults, runtime, seed);

          bestResult.setBestSolutionIdx(0);
          bestResult.setBestRuntime(runtime);
          bestResult.setBestNumHyperedgeCuts(
              currentResults.getBestNumHyperedgeCuts());
          bestResult.setBestNumTerminals(currentResults.getBestNumTerminals());
          bestResult.setBestHopWeigth(currentResults.getBestHopWeigth());
          bestResult.setBestSetSize(currentResults.getBestSetSize());
          bestResult.setBestSetArea(currentResults.getBestSetArea());

          _results.push_back(bestResult);
          currentResults.clearAssignments();
        } else {
          _results.pop_back();
          currentResults.clearAssignments();
        }
      }
      firstRun = firstRun + 1;
      if (firstRun % 100 == 0) {
        std::cout << "Partitioned graph for " << firstRun << " seeds.\n";
      }
    } else {
      std::cout << "Partitioned graph for seed " << seed << " in " << runtime
                << " ms.\n";
    }
  }
  free(vertexWeights);
  free(rowPtr);
  free(colIdx);
  free(edgeWeights);

  if (_options.getSeeds().size() <= 19) {
    _results.push_back(currentResults);
    computePartitionResult(partitionId, evaluationFunction);
  }

  std::cout << "GPMetis run completed. Partition ID = " << partitionId
            << ". Total runs = " << _options.getSeeds().size() << ".\n";
}

void PartClusManagerKernel::runMlPart()
{
  std::cout << "Running MLPart...\n";
  HypergraphDecomposition hypergraphDecomp;
  hypergraphDecomp.init(_dbId);
  Hypergraph hypergraph;
  if (_options.getForceGraph()) {
    hypergraphDecomp.toHypergraph(hypergraph, _graph);
  } else {
    hypergraph = _hypergraph;
  }

  PartSolutions currentResults;
  currentResults.setToolName(_options.getTool());
  unsigned partitionId = generatePartitionId();
  currentResults.setPartitionId(partitionId);
  currentResults.setNumOfRuns(_options.getSeeds().size());
  std::string evaluationFunction = _options.getEvaluationFunction();

  PartSolutions bestResult;
  bestResult.setToolName(_options.getTool());
  bestResult.setPartitionId(partitionId);
  bestResult.setNumOfRuns(1);
  int firstRun = 0;

  int numOriginalVertices = hypergraph.getNumVertex();
  std::vector<unsigned long> clusters(numOriginalVertices, 0);

  double tolerance = _options.getBalanceConstraint() / 100.0;
  double balanceArray[2] = {0.5, 0.5};

  for (long seed : _options.getSeeds()) {
    std::vector<short> partitions;
    int countPartitions = 0;
    partitions.push_back(0);

    auto start = std::chrono::system_clock::now();
    std::time_t startTime = std::chrono::system_clock::to_time_t(start);
    while (partitions.size() < _options.getTargetPartitions()) {
      std::vector<short> auxPartitions;
      for (int p : partitions) {
        Hypergraph newHypergraph;
        countPartitions++;
        hypergraphDecomp.updateHypergraph(
            hypergraph, newHypergraph, clusters, p);
        int numEdges = newHypergraph.getNumEdges();
        int numColIdx = newHypergraph.getNumColIdx();
        int numVertices = newHypergraph.getNumVertex();

        double* vertexWeights
            = (double*) malloc((unsigned) numVertices * sizeof(double));
        int* rowPtr = (int*) malloc((unsigned) (numEdges + 1) * sizeof(int));
        int* colIdx = (int*) malloc((unsigned) numColIdx * sizeof(int));
        double* edgeWeights
            = (double*) malloc((unsigned) numEdges * sizeof(double));
        int* part = (int*) malloc((unsigned) numVertices * sizeof(int));

        for (int j = 0; j < numVertices; j++)
          part[j] = -1;

        for (int i = 0; i < numVertices; i++) {
          vertexWeights[i] = newHypergraph.getVertexWeight(i)
                             / _options.getMaxVertexWeight();
        }
        for (int i = 0; i < numColIdx; i++) {
          colIdx[i] = newHypergraph.getColIdx(i);
        }
        for (int i = 0; i < numEdges; i++) {
          rowPtr[i] = newHypergraph.getRowPtr(i);
          edgeWeights[i] = newHypergraph.getEdgeWeight(i);
        }
        rowPtr[numEdges] = newHypergraph.getRowPtr(numEdges);

        UMpack_mlpart(numVertices,
                      numEdges,
                      vertexWeights,
                      rowPtr,
                      colIdx,
                      edgeWeights,
                      2,  // Number of Partitions
                      balanceArray,
                      tolerance,
                      part,
                      1,  // Starts Per Run #TODO: add a tcl command
                      1,  // Number of Runs
                      0,  // Debug Level
                      seed);
        for (int i = 0; i < numOriginalVertices; i++) {
          if (clusters[i] == p) {
            if (part[newHypergraph.getClusterMapping(i)] == 0)
              clusters[i] = p;
            else
              clusters[i] = countPartitions;
          }
        }
        free(vertexWeights);
        free(rowPtr);
        free(colIdx);
        free(edgeWeights);
        free(part);

        auxPartitions.push_back(countPartitions);
      }

      partitions.insert(
          partitions.end(), auxPartitions.begin(), auxPartitions.end());
    }
    auto end = std::chrono::system_clock::now();
    unsigned long runtime
        = std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
              .count();

    currentResults.addAssignment(clusters, runtime, seed);

    if (_options.getSeeds().size() > 19) {
      if (firstRun <= 0) {
        bestResult.addAssignment(clusters, runtime, seed);
        _results.push_back(bestResult);
        computePartitionResult(partitionId, evaluationFunction);
        bestResult = _results.back();
        currentResults.clearAssignments();
      } else {
        currentResults.setNumOfRuns(1);
        currentResults.setPartitionId(partitionId + 1);
        _results.push_back(currentResults);
        computePartitionResult((partitionId + 1), evaluationFunction);
        currentResults = _results.back();
        bool isNewIdBetter = comparePartitionings(
            bestResult, currentResults, evaluationFunction);
        if (isNewIdBetter) {
          _results.pop_back();
          _results.pop_back();
          bestResult.clearAssignments();
          bestResult.addAssignment(clusters, runtime, seed);

          bestResult.setBestSolutionIdx(0);
          bestResult.setBestRuntime(runtime);
          bestResult.setBestNumHyperedgeCuts(
              currentResults.getBestNumHyperedgeCuts());
          bestResult.setBestNumTerminals(currentResults.getBestNumTerminals());
          bestResult.setBestHopWeigth(currentResults.getBestHopWeigth());
          bestResult.setBestSetSize(currentResults.getBestSetSize());
          bestResult.setBestSetArea(currentResults.getBestSetArea());

          _results.push_back(bestResult);
          currentResults.clearAssignments();
        } else {
          _results.pop_back();
          currentResults.clearAssignments();
        }
      }
      firstRun = firstRun + 1;
      if (firstRun % 100 == 0) {
        std::cout << "Partitioned graph for " << firstRun << " seeds.\n";
      }
    } else {
      std::cout << "Partitioned graph for seed " << seed << " in " << runtime
                << " ms.\n";
    }

    std::fill(clusters.begin(), clusters.end(), 0);
  }

  if (_options.getSeeds().size() <= 19) {
    _results.push_back(currentResults);
    computePartitionResult(partitionId, evaluationFunction);
  }

  std::cout << "MLPart run completed. Partition ID = " << partitionId
            << ". Total runs = " << _options.getSeeds().size() << ".\n";
}

void PartClusManagerKernel::toHypergraph()
{
  HypergraphDecomposition hypergraphDecomp;
  hypergraphDecomp.init(_dbId);
  Hypergraph hype;
  hypergraphDecomp.toHypergraph(hype, _graph);
}

void PartClusManagerKernel::hypergraph()
{
  _hypergraph.fullClearHypergraph();
  HypergraphDecomposition hypergraphDecomp;
  hypergraphDecomp.init(_dbId);
  hypergraphDecomp.constructMap(_hypergraph, _options.getMaxVertexWeight());
  int numVertices = _hypergraph.getNumVertex();
  std::vector<unsigned long> clusters(numVertices, 0);
  hypergraphDecomp.createHypergraph(_hypergraph, clusters, 0);
  toGraph();
}

void PartClusManagerKernel::toGraph()
{
  HypergraphDecomposition hypergraphDecomp;
  hypergraphDecomp.init(_dbId);
  _graph.clearGraph();
  hypergraphDecomp.toGraph(_hypergraph,
                           _graph,
                           _options.getGraphModel(),
                           _options.getWeightModel(),
                           _options.getMaxEdgeWeight(),
                           _options.getCliqueThreshold());
}

unsigned PartClusManagerKernel::generatePartitionId()
{
  unsigned sizeOfResults = _results.size();
  return sizeOfResults;
}

// Evaluate Partitioning

void PartClusManagerKernel::evaluatePartitioning()
{
  std::vector<int> partVector = _options.getPartitionsToTest();
  std::string evaluationFunction = _options.getEvaluationFunction();
  // Checks if IDs are valid
  for (int partId : partVector) {
    if (partId >= _results.size()) {
      std::exit(1);
    }
  }
  int bestId = -1;
  for (int partId : partVector) {
    // Compares the results for the current ID with the best one (if it exists).
    if (bestId == -1) {
      bestId = partId;
    } else {
      // If the new ID presents better results than the last one, update the
      // bestId.
      bool isNewIdBetter = comparePartitionings(getPartitioningResult(bestId),
                                                getPartitioningResult(partId),
                                                evaluationFunction);
      if (isNewIdBetter) {
        bestId = partId;
      }
    }
  }

  reportPartitionResult(bestId);
  setCurrentBestId(bestId);
}

void PartClusManagerKernel::computePartitionResult(unsigned partitionId,
                                                   std::string function)
{
  odb::dbDatabase* db = odb::dbDatabase::getDatabase(_dbId);
  odb::dbChip* chip = db->getChip();
  odb::dbBlock* block = chip->getBlock();
  std::vector<unsigned long> setSizes;
  std::vector<unsigned long> setAreas;
  int weightModel = _options.getWeightModel();
  std::vector<float> edgeWeight = _graph.getDefaultEdgeWeight();
  int maxEdgeWeight = _options.getMaxEdgeWeight();

  float maxEWeight = *std::max_element(edgeWeight.begin(), edgeWeight.end());
  float minEWeight = *std::min_element(edgeWeight.begin(), edgeWeight.end());

  PartSolutions currentResults = _results[partitionId];
  for (unsigned idx = 0; idx < currentResults.getNumOfRuns(); idx++) {
    std::vector<unsigned long> currentAssignment
        = currentResults.getAssignment(idx);
    unsigned long currentRuntime = currentResults.getRuntime(idx);
    int currentSeed = currentResults.getSeed(idx);

    unsigned long terminalCounter = 0;
    unsigned long cutCounter = 0;
    unsigned long edgeTotalWeigth = 0;
    std::vector<unsigned long> setSize(_options.getTargetPartitions(), 0);
    std::vector<unsigned long> setArea(_options.getTargetPartitions(), 0);

    std::vector<int> hyperedgesEnd = _hypergraph.getRowPtr();
    std::vector<int> hyperedgeNets = _hypergraph.getColIdx();
    std::set<unsigned> computedVertices;
    int startIndex = 0;
    for (int endIndex :
         hyperedgesEnd) {  // Iterate over each net in the hypergraph.
      std::set<unsigned long>
          netPartitions;  // Contains the partitions the net is part of.
      std::vector<unsigned>
          netVertices;  // Contains the vertices that are in the net.
      if (endIndex != 0) {
        for (int currentIndex = startIndex; currentIndex < endIndex;
             currentIndex++) {  // Iterate over all vertices in the net.
          int currentVertex = hyperedgeNets[currentIndex];
          unsigned long currentPartition = currentAssignment[currentVertex];
          netPartitions.insert(currentPartition);
          unsigned long currentVertexWeight = _hypergraph.getVertexWeight(idx);
          netVertices.push_back(currentVertex);
          if (computedVertices.find(currentVertex)
              == computedVertices
                     .end()) {  // Update the partition size and area if needed.
            setSize[currentPartition] = setSize[currentPartition] + 1;
            setArea[currentPartition] = setArea[currentPartition]
                                        + _graph.getVertexWeight(currentVertex);
            computedVertices.insert(currentVertex);
          }
        }
      }
      if (netPartitions.size() > 1) {  // Net was cut.
        cutCounter
            = cutCounter + 1;  // If the net was cut, a hyperedge cut happened.
        terminalCounter
            = terminalCounter
              + netPartitions.size();  // The number of different partitions
                                       // present in the net is the number of
                                       // terminals. (Pathways to another set.)
        // Computations for hop weight:
        float currentNetWeight = 0;
        int netSize = netVertices.size();
        switch (weightModel) {
          case 1:
            currentNetWeight = 1.0 / (netSize - 1);
          case 2:
            currentNetWeight = 4.0 / (netSize * (netSize - 1));
          case 3:
            currentNetWeight = 4.0 / (netSize * netSize - (netSize % 2));
          case 4:
            currentNetWeight = 6.0 / (netSize * (netSize + 1));
          case 5:
            currentNetWeight = pow((2.0 / netSize), 1.5);
          case 6:
            currentNetWeight = pow((2.0 / netSize), 3);
          case 7:
            currentNetWeight = 2.0 / netSize;
        }
        int auxWeight;

        currentNetWeight = std::min(currentNetWeight, maxEWeight);
        if (minEWeight == maxEWeight) {
          auxWeight = maxEdgeWeight;
        } else {
          auxWeight
              = (int) ((((currentNetWeight - minEWeight) * (maxEdgeWeight - 1))
                        / (maxEWeight - minEWeight))
                       + 1);
        }

        edgeTotalWeigth = edgeTotalWeigth + auxWeight;
      }
      startIndex = endIndex;
    }

    // Computation for the standard deviation of set size.
    double currentSum = 0;
    for (unsigned long clusterSize : setSize) {
      currentSum = currentSum + clusterSize;
    }
    double currentMean = currentSum / setSize.size();
    double sizeSD = 0;
    for (unsigned long clusterSize : setSize) {
      sizeSD = sizeSD + std::pow(clusterSize - currentMean, 2);
    }
    sizeSD = std::sqrt(sizeSD / setSize.size());

    // Computation for the standard deviation of set area.
    currentSum = 0;
    for (unsigned long clusterArea : setArea) {
      currentSum = currentSum + clusterArea;
    }
    currentMean = currentSum / setArea.size();
    double areaSD = 0;
    for (unsigned long clusterArea : setArea) {
      areaSD = areaSD + std::pow(clusterArea - currentMean, 2);
    }
    areaSD = std::sqrt(areaSD / setArea.size());

    // Check if the current assignment is better than the last one. "terminals
    // hyperedges size area runtime hops"
    bool isBetter = false;
    if (function == "hyperedges") {
      isBetter = ((cutCounter < currentResults.getBestNumHyperedgeCuts())
                  || (currentResults.getBestNumHyperedgeCuts() == 0));
    } else if (function == "terminals") {
      isBetter = ((terminalCounter < currentResults.getBestNumTerminals())
                  || (currentResults.getBestNumTerminals() == 0));
    } else if (function == "size") {
      isBetter = ((sizeSD < currentResults.getBestSetSize())
                  || (currentResults.getBestSetSize() == 0));
    } else if (function == "area") {
      isBetter = ((areaSD < currentResults.getBestSetArea())
                  || (currentResults.getBestSetArea() == 0));
    } else if (function == "hops") {
      isBetter = ((edgeTotalWeigth < currentResults.getBestHopWeigth())
                  || (currentResults.getBestHopWeigth() == 0));
    } else {
      isBetter = ((currentRuntime < currentResults.getBestRuntime())
                  || (currentResults.getBestRuntime() == 0));
    }
    if (isBetter) {
      currentResults.setBestSolutionIdx(idx);
      currentResults.setBestRuntime(currentRuntime);
      currentResults.setBestNumHyperedgeCuts(cutCounter);
      currentResults.setBestNumTerminals(terminalCounter);
      currentResults.setBestHopWeigth(edgeTotalWeigth);
      currentResults.setBestSetSize(sizeSD);
      currentResults.setBestSetArea(areaSD);
    }
  }
  _results[partitionId] = currentResults;
}

bool PartClusManagerKernel::comparePartitionings(PartSolutions oldPartition,
                                                 PartSolutions newPartition,
                                                 std::string function)
{
  bool isBetter = false;
  if (function == "hyperedges") {
    isBetter = newPartition.getBestNumHyperedgeCuts()
               < oldPartition.getBestNumHyperedgeCuts();
  } else if (function == "terminals") {
    isBetter = newPartition.getBestNumTerminals()
               < oldPartition.getBestNumTerminals();
  } else if (function == "size") {
    isBetter = newPartition.getBestSetSize() < oldPartition.getBestSetSize();
  } else if (function == "area") {
    isBetter = newPartition.getBestSetArea() < oldPartition.getBestSetArea();
  } else if (function == "hops") {
    isBetter
        = newPartition.getBestHopWeigth() < oldPartition.getBestHopWeigth();
  } else {
    isBetter = newPartition.getBestRuntime() < oldPartition.getBestRuntime();
  }
  return isBetter;
}

void PartClusManagerKernel::reportPartitionResult(unsigned partitionId)
{
  PartSolutions currentResults = _results[partitionId];
  std::cout << "\nPartitioning Results for ID = " << partitionId
            << " and Tool = " << currentResults.getToolName() << ".\n";
  unsigned bestIdx = currentResults.getBestSolutionIdx();
  int seed = currentResults.getSeed(bestIdx);
  std::cout << "Best results used seed " << seed << ".\n";
  std::cout << "Number of Hyperedge Cuts = "
            << currentResults.getBestNumHyperedgeCuts() << ".\n";
  std::cout << "Number of Terminals = " << currentResults.getBestNumTerminals()
            << ".\n";
  std::cout << "Cluster Size SD = " << currentResults.getBestSetSize() << ".\n";
  std::cout << "Cluster Area SD = " << currentResults.getBestSetArea() << ".\n";
  std::cout << "Total Hop Weigth = " << currentResults.getBestHopWeigth()
            << ".\n";
  std::cout << "Total Runtime = " << currentResults.getBestRuntime() << ".\n\n";
}

// Write Partitioning To DB

odb::dbBlock* PartClusManagerKernel::getDbBlock() const
{
  odb::dbDatabase* db = odb::dbDatabase::getDatabase(_dbId);
  odb::dbChip* chip = db->getChip();
  odb::dbBlock* block = chip->getBlock();
  return block;
}

void PartClusManagerKernel::writePartitioningToDb(unsigned partitioningId)
{
  std::cout << "[INFO] Writing partition id's to DB.\n";
  if (partitioningId >= getNumPartitioningResults()) {
    std::cout << "[ERROR] Partition id out of range (" << partitioningId
              << ").\n";
    return;
  }

  PartSolutions& results = getPartitioningResult(partitioningId);
  unsigned bestSolutionIdx = results.getBestSolutionIdx();
  const std::vector<unsigned long>& result
      = results.getAssignment(bestSolutionIdx);

  odb::dbBlock* block = getDbBlock();
  for (odb::dbInst* inst : block->getInsts()) {
    std::string instName = inst->getName();
    int instIdx = _hypergraph.getMapping(instName);
    unsigned long partitionId = result[instIdx];

    odb::dbIntProperty* propId = odb::dbIntProperty::find(inst, "partition_id");
    if (!propId) {
      propId = odb::dbIntProperty::create(inst, "partition_id", partitionId);
    } else {
      propId->setValue(partitionId);
    }
  }

  std::cout << "[INFO] Writing done.\n";
}

void PartClusManagerKernel::dumpPartIdToFile(std::string name)
{
  std::ofstream file(name);

  odb::dbBlock* block = getDbBlock();
  for (odb::dbInst* inst : block->getInsts()) {
    std::string instName = inst->getName();
    odb::dbIntProperty* propId = odb::dbIntProperty::find(inst, "partition_id");
    if (!propId) {
      std::cout << "[ERROR] Property not found for inst " << instName << "\n";
      continue;
    }
    file << instName << " " << propId->getValue() << "\n";
  }

  file.close();
}

// Cluster Netlist

void PartClusManagerKernel::runClustering()
{
  hypergraph();
  if (_options.getTool() == "mlpart") {
    runMlPartClustering();
  } else if (_options.getTool() == "gpmetis") {
    runGpMetisClustering();
  } else {
    runChacoClustering();
  }
}

void PartClusManagerKernel::runChacoClustering()
{
  std::cout << "\nRunning chaco...\n";

  PartSolutions currentResults;
  currentResults.setToolName(_options.getTool());
  unsigned clusterId = generateClusterId();
  currentResults.setPartitionId(clusterId);
  currentResults.setNumOfRuns(_options.getSeeds().size());
  std::string evaluationFunction = _options.getEvaluationFunction();

  std::vector<int> edgeWeights = _graph.getEdgeWeight();
  std::vector<int> vertexWeights = _graph.getVertexWeight();
  std::vector<int> colIdx = _graph.getColIdx();
  std::vector<int> rowPtr = _graph.getRowPtr();
  int numVertices = vertexWeights.size();

  int architecture = _options.getArchTopology().size();
  int architectureDims = 1;
  int* mesh_dims = (int*) malloc((unsigned) 3 * sizeof(int));

  int numVertCoar = _options.getCoarVertices();

  int refinement = _options.getRefinement();

  double inbalance = (double) _options.getBalanceConstraint() / 100;

  double coarRatio = _options.getCoarRatio();

  double cutCost = _options.getCutHopRatio();

  int level = _options.getLevel();

  auto start = std::chrono::system_clock::now();
  std::time_t startTime = std::chrono::system_clock::to_time_t(start);

  int* starts = (int*) malloc((unsigned) (numVertices + 1) * sizeof(int));
  int* currentIndex = starts;
  for (int pointer : rowPtr) {
    *currentIndex = (pointer);
    currentIndex++;
  }
  *currentIndex = colIdx.size();  // Needed so Chaco can find the end of the
                                  // interval of the last vertex

  int* vweights = (int*) malloc((unsigned) numVertices * sizeof(int));
  currentIndex = vweights;
  for (int weigth : vertexWeights) {
    *currentIndex = weigth;
    currentIndex++;
  }

  int* adjacency = (int*) malloc((unsigned) colIdx.size() * sizeof(int));
  currentIndex = adjacency;
  for (int pointer : colIdx) {
    *currentIndex = (pointer + 1);
    currentIndex++;
  }

  float* eweights = (float*) malloc((unsigned) colIdx.size() * sizeof(float));
  float* currentIndexFloat = eweights;
  for (int weigth : edgeWeights) {
    *currentIndexFloat = weigth;
    currentIndexFloat++;
  }

  short* assigment = (short*) malloc((unsigned) numVertices * sizeof(short));

  interface_wrap(
      numVertices, /* number of vertices */
      starts,
      adjacency,
      vweights,
      eweights, /* graph definition for chaco */
      NULL,
      NULL,
      NULL, /* x y z positions for the inertial method, not needed for
               multi-level KL */
      NULL,
      NULL, /* output assigment name and file, isn't needed because internal
               methods of PartClusManager are used instead */
      assigment, /* vertex assigment vector. Contains the set that each vector
                    is present on.*/
      architecture,
      1,
      mesh_dims, /* architecture, architecture topology and the hypercube
                    dimensions (number of 2-way divisions) */
      NULL, /* desired set sizes for each set, computed automatically, so it
               isn't needed */
      1,
      1, /* constants that define the methods used by the partitioner ->
            multi-level KL, KL refinement */
      0,
      numVertCoar,
      1, /* disables the eigensolver, number of vertices to coarsen down to and
            bisection/quadrisection/octasection */
      0.001,
      0, /* tolerance on eigenvectors (hard-coded, not used) and the seed */
      0,
      inbalance, /* terminal propagation enable and inbalance */
      coarRatio,
      cutCost, /* coarsening ratio and cut to hop cost */
      0,
      refinement,
      level); /* debug text enable, refinement and clustering level to export*/

  std::vector<unsigned long> chacoResult;

  int* clusteringResults = clustering_wrap();
  for (int i = 0; i < numVertices; i++) {
    int* currentpointer = (clusteringResults + 1) + i;
    chacoResult.push_back(*currentpointer);
  }
  free(clusteringResults);

  auto end = std::chrono::system_clock::now();
  unsigned long runtime
      = std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
            .count();

  currentResults.addAssignment(chacoResult, runtime, 0);
  free(assigment);
  std::cout << "Clustered graph in " << runtime << " ms.\n";

  _clusResults.push_back(currentResults);

  free(mesh_dims);

  std::cout << "Chaco run completed. Cluster ID = " << clusterId << ".\n";
}

void PartClusManagerKernel::runGpMetisClustering()
{
  std::cout << "Running GPMetis...\n";
  PartSolutions currentResults;
  currentResults.setToolName(_options.getTool());
  unsigned clusterId = generateClusterId();
  currentResults.setPartitionId(clusterId);
  currentResults.setNumOfRuns(_options.getSeeds().size());
  std::string evaluationFunction = _options.getEvaluationFunction();

  idx_t edgeCut;
  idx_t nPartitions = _options.getTargetPartitions();
  int numVertices = _graph.getNumVertex();
  int numEdges = _graph.getNumEdges();
  idx_t constraints = 1;
  idx_t options[METIS_NOPTIONS];
  METIS_SetDefaultOptions(options);
  idx_t level = _options.getLevel();

  options[METIS_OPTION_PTYPE] = METIS_PTYPE_RB;
  options[METIS_OPTION_OBJTYPE] = METIS_OBJTYPE_CUT;
  options[METIS_OPTION_NUMBERING] = 0;
  options[METIS_OPTION_UFACTOR] = _options.getBalanceConstraint() * 10;

  idx_t* vertexWeights
      = (idx_t*) malloc((unsigned) numVertices * sizeof(idx_t));
  idx_t* rowPtr = (idx_t*) malloc((unsigned) (numVertices + 1) * sizeof(idx_t));
  idx_t* colIdx = (idx_t*) malloc((unsigned) numEdges * sizeof(idx_t));
  idx_t* edgeWeights = (idx_t*) malloc((unsigned) numEdges * sizeof(idx_t));
  for (int i = 0; i < numVertices; i++) {
    vertexWeights[i] = _graph.getVertexWeight(i);
    edgeWeights[i] = _graph.getEdgeWeight(i);
    rowPtr[i] = _graph.getRowPtr(i);
    colIdx[i] = _graph.getColIdx(i);
  }
  rowPtr[numVertices] = numEdges;

  for (int i = numVertices; i < numEdges; i++) {
    edgeWeights[i] = _graph.getEdgeWeight(i);
    colIdx[i] = _graph.getColIdx(i);
  }
  std::vector<unsigned long> gpmetisResults;
  auto start = std::chrono::system_clock::now();
  std::time_t startTime = std::chrono::system_clock::to_time_t(start);
  idx_t* parts = (idx_t*) malloc((unsigned) numVertices * sizeof(idx_t));

  METIS_CoarsenGraph(&numVertices,
                     &constraints,
                     rowPtr,
                     colIdx,
                     vertexWeights,
                     NULL,
                     edgeWeights,
                     &nPartitions,
                     NULL,
                     NULL,
                     options,
                     &edgeCut,
                     parts,
                     &level);

  for (int i = 0; i < numVertices; i++)
    gpmetisResults.push_back(parts[i]);
  auto end = std::chrono::system_clock::now();
  unsigned long runtime
      = std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
            .count();

  std::cout << "Clustered graph in " << runtime << " ms.\n";
  currentResults.addAssignment(gpmetisResults, runtime, 0);
  _clusResults.push_back(currentResults);
  free(parts);
  free(vertexWeights);
  free(rowPtr);
  free(colIdx);
  free(edgeWeights);

  std::cout << "GPMetis run completed. Cluster ID = " << clusterId << ".\n";
}

void PartClusManagerKernel::runMlPartClustering()
{
  std::cout << "Running MLPart...\n";
  HypergraphDecomposition hypergraphDecomp;
  hypergraphDecomp.init(_dbId);
  Hypergraph hypergraph;
  if (_options.getForceGraph()) {
    hypergraphDecomp.toHypergraph(hypergraph, _graph);
  } else {
    hypergraph = _hypergraph;
  }

  PartSolutions currentResults;
  currentResults.setToolName(_options.getTool());
  unsigned clusterId = generateClusterId();
  currentResults.setPartitionId(clusterId);
  currentResults.setNumOfRuns(_options.getSeeds().size());
  std::string evaluationFunction = _options.getEvaluationFunction();
  unsigned level = _options.getLevel();

  std::vector<unsigned long> clusters;

  double tolerance = _options.getBalanceConstraint() / 100.0;
  double balanceArray[2] = {0.5, 0.5};

  auto start = std::chrono::system_clock::now();
  std::time_t startTime = std::chrono::system_clock::to_time_t(start);
  int numEdges = hypergraph.getNumEdges();
  int numColIdx = hypergraph.getNumColIdx();
  int numVertices = hypergraph.getNumVertex();

  double* vertexWeights
      = (double*) malloc((unsigned) numVertices * sizeof(double));
  int* rowPtr = (int*) malloc((unsigned) (numEdges + 1) * sizeof(int));
  int* colIdx = (int*) malloc((unsigned) numColIdx * sizeof(int));
  double* edgeWeights = (double*) malloc((unsigned) numEdges * sizeof(double));
  int* part = (int*) malloc((unsigned) numVertices * sizeof(int));

  for (int j = 0; j < numVertices; j++)
    part[j] = -1;

  for (int i = 0; i < numVertices; i++) {
    vertexWeights[i]
        = hypergraph.getVertexWeight(i) / _options.getMaxVertexWeight();
  }
  for (int i = 0; i < numColIdx; i++) {
    colIdx[i] = hypergraph.getColIdx(i);
  }
  for (int i = 0; i < numEdges; i++) {
    rowPtr[i] = hypergraph.getRowPtr(i);
    edgeWeights[i] = hypergraph.getEdgeWeight(i);
  }
  rowPtr[numEdges] = hypergraph.getRowPtr(numEdges);

  UMpack_mlpart(numVertices,
                numEdges,
                vertexWeights,
                rowPtr,
                colIdx,
                edgeWeights,
                2,  // Number of Partitions
                balanceArray,
                tolerance,
                part,
                1,  // Starts Per Run #TODO: add a tcl command
                1,  // Number of Runs
                0,  // Debug Level
                123,
                level);
  for (int i = 0; i < numVertices; i++) {
    clusters.push_back(part[i]);
  }

  auto end = std::chrono::system_clock::now();
  unsigned long runtime
      = std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
            .count();
  std::cout << "Clustered graph in " << runtime << " ms.\n";
  free(vertexWeights);
  free(rowPtr);
  free(colIdx);
  free(edgeWeights);
  free(part);

  currentResults.addAssignment(clusters, runtime, 0);

  _clusResults.push_back(currentResults);

  std::cout << "MLPart run completed. Cluster ID = " << clusterId << ".\n";
}

unsigned PartClusManagerKernel::generateClusterId()
{
  unsigned sizeOfResults = _clusResults.size();
  return sizeOfResults;
}

// Write Clustering To DB

void PartClusManagerKernel::writeClusteringToDb(unsigned clusteringId)
{
  std::cout << "[INFO] Writing cluster id's to DB.\n";
  if (clusteringId >= getNumClusteringResults()) {
    std::cout << "[ERROR] Cluster id out of range (" << clusteringId << ").\n";
    return;
  }

  PartSolutions& results = getClusteringResult(clusteringId);
  const std::vector<unsigned long>& result
      = results.getAssignment(0);  // Clustering uses only 1 seed

  odb::dbBlock* block = getDbBlock();
  for (odb::dbInst* inst : block->getInsts()) {
    std::string instName = inst->getName();
    int instIdx = _hypergraph.getMapping(instName);
    unsigned long clusterId = result[instIdx];

    odb::dbIntProperty* propId = odb::dbIntProperty::find(inst, "cluster_id");
    if (!propId) {
      propId = odb::dbIntProperty::create(inst, "cluster_id", clusterId);
    } else {
      propId->setValue(clusterId);
    }
  }

  std::cout << "[INFO] Writing done.\n";
}

void PartClusManagerKernel::dumpClusIdToFile(std::string name)
{
  std::ofstream file(name);

  odb::dbBlock* block = getDbBlock();
  for (odb::dbInst* inst : block->getInsts()) {
    std::string instName = inst->getName();
    odb::dbIntProperty* propId = odb::dbIntProperty::find(inst, "cluster_id");
    if (!propId) {
      std::cout << "[ERROR] Property not found for inst " << instName << "\n";
      continue;
    }
    file << instName << " " << propId->getValue() << "\n";
  }

  file.close();
}

// Report Netlist Partitions

void PartClusManagerKernel::reportNetlistPartitions(unsigned partitionId)
{
  std::map<unsigned long, unsigned long> setSizes;
  std::set<unsigned long> partitions;
  unsigned long numberOfPartitions = 0;
  PartSolutions& results = getPartitioningResult(partitionId);
  unsigned bestSolutionIdx = results.getBestSolutionIdx();
  const std::vector<unsigned long>& result
      = results.getAssignment(bestSolutionIdx);
  for (unsigned long currentPartition : result) {
    if (currentPartition > numberOfPartitions) {
      numberOfPartitions = currentPartition;
    }
    if (setSizes.find(currentPartition) == setSizes.end()) {
      setSizes[currentPartition] = 1;
    } else {
      setSizes[currentPartition] = setSizes[currentPartition] + 1;
    }
    partitions.insert(currentPartition);
  }
  std::cout << "[REPORT NETLIST] \nThe netlist has " << (numberOfPartitions + 1)
            << " partitions.\n\n";
  unsigned long totalVertices = 0;
  for (unsigned long partIdx : partitions) {
    unsigned long partSize = setSizes[partIdx];
    std::cout << "Partition " << partIdx << " has " << partSize
              << " vertices.\n";
    totalVertices = totalVertices + partSize;
  }
  std::cout << "\nThe total number of vertices is " << totalVertices
            << ".\n[REPORT NETLIST END]\n";
}

// Read partitioning input file

void PartClusManagerKernel::readPartitioningFile(std::string filename)
{
  hypergraph();
  PartSolutions currentResults;
  currentResults.setToolName(_options.getTool());
  unsigned partitionId = generatePartitionId();
  currentResults.setPartitionId(partitionId);
  currentResults.setNumOfRuns(1);
  std::string evaluationFunction = _options.getEvaluationFunction();
  _options.setTargetPartitions(_options.getFinalPartitions());

  std::ifstream file(filename);
  std::string line;
  std::vector<unsigned long> partitions;
  if (file.is_open()) {
    while (getline(file, line)) {
      partitions.push_back(std::stoi(line));
    }
    file.close();
  } else {
    std::cout << "Unable to open file\n";
  }
  currentResults.addAssignment(partitions, 0, 1);
  _results.push_back(currentResults);
  computePartitionResult(partitionId, evaluationFunction);
}

}  // namespace PartClusManager
