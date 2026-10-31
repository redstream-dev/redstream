/* The boxes returned by FixtureBoxes in the test parser module (infer-parser-fixture.cpp), for tests that assert
 * on what rdsinfer makes of them. C and C++.
 *
 * Network-input pixels, inside 64x64 (use a network at least that big):
 *   0: class 0, (4, 4) 16x16, confidence 0.90
 *   1: class 0, (6, 6) 16x16, confidence 0.80: overlaps box 0 (IoU 196 / 316 = 0.62), so NMS at IoU 0.5 keeps
 *      only box 0
 *   2: class 1, (40, 8) 12x20, confidence 0.70
 *   3: class 1, (20, 40) 10x10, confidence 0.10: below a 0.2 threshold (the parser does not filter it)
 * rotation_angle is 0 for all of them.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_TEST_INFER_PARSER_FIXTURE_H
#define RDS_TEST_INFER_PARSER_FIXTURE_H

typedef struct
{
  unsigned int class_id;
  float left, top, width, height, confidence;
} RdsTestFixtureBox;

#define RDS_TEST_FIXTURE_N_BOXES 4

static const RdsTestFixtureBox rds_test_fixture_boxes[RDS_TEST_FIXTURE_N_BOXES] = {
  { 0, 4.0f, 4.0f, 16.0f, 16.0f, 0.9f },
  { 0, 6.0f, 6.0f, 16.0f, 16.0f, 0.8f },
  { 1, 40.0f, 8.0f, 12.0f, 20.0f, 0.7f },
  { 1, 20.0f, 40.0f, 10.0f, 10.0f, 0.1f },
};

/* FixtureClassify's attributes, one per output layer i:
 *   attributeIndex i, attributeValue round (RDS_TEST_FIXTURE_ATTR_SCALE x the layer's first value),
 *   attributeConfidence classifierThreshold / 2 (below the threshold: rdsinfer applies none to a custom parser),
 *   attributeLabel "attr<i>-<network width>x<network height>", strdup'ed, except layer 1's: NULL;
 * descString "desc<number of layers>"; FixtureClassifyNoDesc leaves descString empty. FixtureClassifyPastLayers
 * returns one attribute: index = the number of layers (past them), value 5, confidence 0.5, label and descString
 * "past". FixtureClassifyFailDark returns false when output layer 0's first value is
 * below RDS_TEST_FIXTURE_DARK_BELOW. */
#define RDS_TEST_FIXTURE_ATTR_SCALE 256.0f
#define RDS_TEST_FIXTURE_DARK_BELOW 0.25f

/* What FixtureThrow and FixtureClassifyThrow throw: a std::runtime_error with this what(). */
#define RDS_TEST_FIXTURE_THROW_WHAT "fixture parser exception"

#endif /* RDS_TEST_INFER_PARSER_FIXTURE_H */
