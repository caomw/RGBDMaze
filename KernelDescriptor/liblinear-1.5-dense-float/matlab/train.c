#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "linear.h"

#include "mex.h"
#include "linear_model_matlab.h"

#include <time.h>

#ifdef DEBUG
void wait ( int seconds )
{
  clock_t endwait;
  endwait = clock () + seconds * CLOCKS_PER_SEC ;
  while (clock() < endwait) {}
}
#endif

#if MX_API_VER < 0x07030000
typedef int mwIndex;
#endif

#define CMD_LEN 2048
#define Malloc(type,n) (type *)malloc((n)*sizeof(type))
#define INF HUGE_VAL

void print_null(const char *s){}

void (*liblinear_default_print_string) (const char *);

void exit_with_help()
{
	mexPrintf(
	"Usage: model = train(training_label_vector, training_instance_matrix, 'liblinear_options', 'col', 'delete x');\n"
#ifdef _DENSE_REP
	"  xren: passing a variable name as 5th argument ==> it will be cleared in matlab once data is copied.\n"
	" ( warning : training_instance_matrix must be dense )\n"
#else
	"  xren: warning: sparse version not yet supported.\n"
#endif
	"liblinear_options:\n"
	"-s type : set type of solver (default 1)\n"
	"	0 -- L2-regularized logistic regression\n"
	"	1 -- L2-regularized L2-loss support vector classification (dual)\n"	
	"	2 -- L2-regularized L2-loss support vector classification (primal)\n"
	"	3 -- L2-regularized L1-loss support vector classification (dual)\n"
	"	4 -- multi-class support vector classification by Crammer and Singer\n"
	"	5 -- L1-regularized L2-loss support vector classification\n"
	"	6 -- L1-regularized logistic regression\n"
	"-c cost : set the parameter C (default 1)\n"
	"-e epsilon : set tolerance of termination criterion\n"
	"	-s 0 and 2\n" 
	"		|f'(w)|_2 <= eps*min(pos,neg)/l*|f'(w0)|_2,\n" 
	"		where f is the primal function, (default 0.01)\n"
	"	-s 1, 3, and 4\n"
	"		Dual maximal violation <= eps; similar to libsvm (default 0.1)\n"
	"	-s 5 and 6\n"
	"		|f'(w)|_inf <= eps*min(pos,neg)/l*|f'(w0)|_inf,\n"
	"		where f is the primal function (default 0.01)\n"
	"-B bias : if bias >= 0, instance x becomes [x; bias]; if < 0, no bias term added (default -1)\n"
	"-wi weight: weights adjust the parameter C of different classes (see README for details)\n"
	"-v n: n-fold cross validation mode\n"
	"-q : quiet mode (no outputs)\n"
	"col:\n"
	"	if 'col' is setted, training_instance_matrix is parsed in column format, otherwise is in row format\n"
	);
}

// liblinear arguments
struct parameter param;		// set by parse_command_line
struct problem prob;		// set by read_problem
struct model *model_;

#ifdef _DENSE_REP
float *x_space;
#else
struct feature_node *x_space;
#endif

int cross_validation_flag;
int col_format_flag;
int nr_fold;
double bias;

double do_cross_validation()
{
	int i;
	int total_correct = 0;
	int *target = Malloc(int,prob.l);
	double retval = 0.0;

	cross_validation(&prob,&param,nr_fold,target);

	for(i=0;i<prob.l;i++)
		if(target[i] == prob.y[i])
			++total_correct;
	mexPrintf("Cross Validation Accuracy = %g%%\n",100.0*total_correct/prob.l);
	retval = 100.0*total_correct/prob.l;

	free(target);
	return retval;
}

// nrhs should be 3
int parse_command_line(int nrhs, const mxArray *prhs[], char *model_file_name)
{
	int i, argc = 1;
	char cmd[CMD_LEN];
	char *argv[CMD_LEN/2];

	// default values
	param.solver_type = L2R_L2LOSS_SVC_DUAL;
	param.C = 1;
	param.eps = INF; // see setting below
	param.nr_weight = 0;
	param.weight_label = NULL;
	param.weight = NULL;
	cross_validation_flag = 0;
	col_format_flag = 0;
	bias = -1;

	// train loaded only once under matlab
	if(liblinear_default_print_string == NULL)
		liblinear_default_print_string = liblinear_print_string;
	else
		liblinear_print_string = liblinear_default_print_string;

	if(nrhs <= 1)
		return 1;

	if(nrhs >= 4)
	{
		mxGetString(prhs[3], cmd, mxGetN(prhs[3])+1);
		if(strcmp(cmd, "col") == 0)
			col_format_flag = 1;
	}

	// put options in argv[]
	if(nrhs > 2)
	{
		mxGetString(prhs[2], cmd,  mxGetN(prhs[2]) + 1);
		if((argv[argc] = strtok(cmd, " ")) != NULL)
			while((argv[++argc] = strtok(NULL, " ")) != NULL)
				;
	}

	// parse options
	for(i=1;i<argc;i++)
	{
		if(argv[i][0] != '-') break;
		++i;
		if(i>=argc && argv[i-1][1] != 'q') // since option -q has no parameter
			return 1;
		switch(argv[i-1][1])
		{
			case 's':
				param.solver_type = atoi(argv[i]);
				break;
			case 'c':
				param.C = atof(argv[i]);
				break;
			case 'e':
				param.eps = atof(argv[i]);
				break;
			case 'B':
				bias = atof(argv[i]);
				break;
			case 'v':
				cross_validation_flag = 1;
				nr_fold = atoi(argv[i]);
				if(nr_fold < 2)
				{
					mexPrintf("n-fold cross validation: n must >= 2\n");
					return 1;
				}
				break;
			case 'w':
				++param.nr_weight;
				param.weight_label = (int *) realloc(param.weight_label,sizeof(int)*param.nr_weight);
				param.weight = (double *) realloc(param.weight,sizeof(double)*param.nr_weight);
				param.weight_label[param.nr_weight-1] = atoi(&argv[i-1][2]);
				param.weight[param.nr_weight-1] = atof(argv[i]);
				break;
			case 'q':
				liblinear_print_string = &print_null;
				i--;
				break;
			default:
				mexPrintf("unknown option\n");
				return 1;
		}
	}

	if(param.eps == INF)
	{
		if(param.solver_type == L2R_LR || param.solver_type == L2R_L2LOSS_SVC)
			param.eps = 0.01;
		else if(param.solver_type == L2R_L2LOSS_SVC_DUAL || param.solver_type == L2R_L1LOSS_SVC_DUAL || param.solver_type == MCSVM_CS)
			param.eps = 0.1;
		else if(param.solver_type == L1R_L2LOSS_SVC || param.solver_type == L1R_LR)
			param.eps = 0.01;
	}
	return 0;
}

static void fake_answer(mxArray *plhs[])
{
	plhs[0] = mxCreateDoubleMatrix(0, 0, mxREAL);
}

int read_problem_sparse(const mxArray *label_vec, const mxArray *instance_mat)
{
#ifdef DEBUG
	mexPrintf("read_problem_sparse...\n");
#endif
	int i, j;
#ifdef _DENSE_REP
#else
	int  k, low, high;
	mwIndex *ir, *jc;
#endif
	size_t elements;
	int max_index, label_vector_row_num;
	double *labels;
	float *samples;
	// mxArray *instance_mat_col; // instance sparse matrix in column format

	prob.x = NULL;
	prob.y = NULL;
	x_space = NULL;

#ifdef DEBUG
	mexPrintf("before assigning to col..\n");
	mexEvalString("drawnow");
	wait(5);
#endif

/*
	mxArray *prhs[1], *plhs[1];
	if(col_format_flag)
		instance_mat_col = (mxArray *)instance_mat;
	else
	{
		// transpose instance matrix
		//prhs[0] = mxDuplicateArray(instance_mat);
		//if(mexCallMATLAB(1, plhs, 1, prhs, "transpose"))
		prhs[0] = (mxArray*)(instance_mat);
		if(mexCallMATLAB(1, plhs, 1, prhs, "transpose"))
		{
			mexPrintf("Error: cannot transpose training instance matrix\n");
			return -1;
		}
		instance_mat_col = plhs[0];
		//mxDestroyArray(prhs[0]);
	}
	mxDestroyArray(plhs[0]);
*/

	// the number of instance
        if (col_format_flag) {
	  prob.l = (int) mxGetN(instance_mat);
        } else {
	  prob.l = (int) mxGetM(instance_mat);
        }
	label_vector_row_num = (int) mxGetM(label_vec);

	if(label_vector_row_num!=prob.l)
	{
		//mexPrintf("Length of label vector does not match # of instances.\n");
		mexPrintf("Length of label vector does not match # of instances %d %d %d.\n",label_vector_row_num,prob.l,col_format_flag);
		return -1;
	}
	
	// each column is one instance
	labels = mxGetPr(label_vec);

	if( !mxIsSingle(instance_mat) ) {
	  mexPrintf("Error: instance matrix needs to be single precision\n");
	  return -1;
	}

	samples = (float*)mxGetPr(instance_mat);

#ifdef _DENSE_REP
        if (col_format_flag) {
	  max_index = (int) mxGetM(instance_mat);
        } else {
	  max_index = (int) mxGetN(instance_mat);
        }

	if(bias >= 0)	  
	  elements = (size_t)(max_index + 1)*(size_t)label_vector_row_num; // with the bias
	else
	  elements = (size_t)max_index*(size_t)label_vector_row_num; // with the bias

#ifdef DEBUG
	mexPrintf("before allocating...\n");
	mexEvalString("drawnow");
	wait(5);
#endif
	//mexPrintf("elements : %i, max_index : %i \n", elements, max_index); 
	prob.y = Malloc(int, prob.l);
	prob.x = Malloc(float *, prob.l);
	x_space = Malloc(float, elements);
	prob.bias = bias;

	size_t x_space_idx = 0, sample_idx  = 0;

	for(i=0;i<prob.l;i++)
        {
	        prob.x[i] = &x_space[x_space_idx];
		prob.y[i] = (int)labels[i];
		for(j=0;j<max_index;j++)
		{
		   if(col_format_flag) {
	               x_space[x_space_idx++] = (float)samples[sample_idx++];
	               //x_space[x_space_idx++] = (float)samples[ i*label_vector_row_num+j ];
		   } else {
	               x_space[x_space_idx++] = (float)samples[ j*label_vector_row_num+i ];
		   }
		}
		if(prob.bias >= 0)
		{
                       x_space[x_space_idx++] = (float)prob.bias;
		}
	}

//xren
//mexPrintf("check: %f %llu %f\n", labels[100000-1], prob.y[100000-1],prob.x[10000-1][1000-1]);
//mexEvalString("drawnow");

#else
	ir = mxGetIr(instance_mat_col);
	jc = mxGetJc(instance_mat_col);

	int num_samples = (int) mxGetNzmax(instance_mat_col);

	elements = num_samples + prob.l*2;
	max_index = (int) mxGetM(instance_mat_col);

	prob.y = Malloc(int, prob.l);
	prob.x = Malloc(struct feature_node*, prob.l);
	x_space = Malloc(struct feature_node, elements);

	prob.bias=bias;

	j = 0;
	for(i=0;i<prob.l;i++)
	{
		prob.x[i] = &x_space[j];
		prob.y[i] = (int) labels[i];
		low = (int) jc[i], high = (int) jc[i+1];
		for(k=low;k<high;k++)
		{
			x_space[j].index = (int) ir[k]+1;
			x_space[j].value = samples[k];
			j++;
	 	}
		if(prob.bias>=0)
		{
			x_space[j].index = max_index+1;
			x_space[j].value = prob.bias;
			j++;
		}
		x_space[j++].index = -1;
	}
#endif
	if(prob.bias>=0)
		prob.n = max_index+1;
	else
		prob.n = max_index;


	return 0;
}

// Interface function of matlab
// now assume prhs[0]: label prhs[1]: features
void mexFunction( int nlhs, mxArray *plhs[],
		int nrhs, const mxArray *prhs[] )
{
	const char *error_msg;
	// fix random seed to have same results for each run
	// (for cross validation)
	srand(1);

	// Transform the input Matrix to libsvm format
	if(nrhs > 0 && nrhs <= 5)
	{
		int err=0;

		//if(!mxIsDouble(prhs[0]) || !mxIsDouble(prhs[1])) {
		if(!mxIsDouble(prhs[0])) {
			mexPrintf("Error: label vector and instance matrix must be double\n");
			fake_answer(plhs);
			return;
		}

		if(parse_command_line(nrhs, prhs, NULL))
		{
			exit_with_help();
			destroy_param(&param);
			fake_answer(plhs);
			return;
		}
#ifdef DEBUG
	mexPrintf("reading problem sparse...\n");
	mexEvalString("drawnow");
	wait(5);
#endif

#ifdef _DENSE_REP
		if(!mxIsSparse(prhs[1]))
			err = read_problem_sparse(prhs[0], prhs[1]);
		else
		{
			mexPrintf("Training_instance_matrix must be dense\n");
			destroy_param(&param);
			fake_answer(plhs);
			return;
		}
#else
		if(mxIsSparse(prhs[1]))
			err = read_problem_sparse(prhs[0], prhs[1]);
		else
		{
			mexPrintf("Training_instance_matrix must be sparse\n");
			destroy_param(&param);
			fake_answer(plhs);
			return;
		}
#endif

#ifdef DEBUG
	mexPrintf("read_problem_sparse done.\n");
	mexEvalString("drawnow");
	wait(5);
#endif

		// xren: delete the input instance matrix to free up space
		if (nrhs==5) {
			mxArray* var=(mxArray*)prhs[4];
			int status=mexCallMATLAB(0,NULL,1, &var, "clear");
			if (status!=0) mexPrintf("Failed to delete variable %s\n",mxArrayToString(prhs[4]));
			//mxDestroyArray( (mxArray*)prhs[1] );
		}
#ifdef DEBUG
	mexPrintf("free memory done.\n");
	mexEvalString("drawnow");
	wait(5);
#endif
		// train's original code
		error_msg = check_parameter(&prob, &param);

		if(err || error_msg)
		{
			if (error_msg != NULL)
				mexPrintf("Error: %s\n", error_msg);
			destroy_param(&param);
			free(prob.y);
			free(prob.x);
			free(x_space);
			fake_answer(plhs);
			return;
		}

		if(cross_validation_flag)
		{
			double *ptr;
			plhs[0] = mxCreateDoubleMatrix(1, 1, mxREAL);
			ptr = mxGetPr(plhs[0]);
			ptr[0] = do_cross_validation();
		}
		else
		{
			const char *error_msg;
			model_ = train(&prob, &param);
			error_msg = model_to_matlab_structure(plhs, model_);
			if(error_msg)
				mexPrintf("Error: can't convert libsvm model to matrix structure: %s\n", error_msg);
			destroy_model(model_);
		}
		destroy_param(&param);
		free(prob.y);
		free(prob.x);
		free(x_space);
	}
	else
	{
		exit_with_help();
		fake_answer(plhs);
		return;
	}
}
